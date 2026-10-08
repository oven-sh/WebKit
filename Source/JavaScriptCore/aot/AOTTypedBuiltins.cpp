/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedBuiltins.h"

#if ENABLE(AOT)

#include "GetterSetter.h"
#include "JSCInlines.h"
#include "MathCommon.h"
#include "PageCount.h"
#include "PureNaN.h"
#include "Symbol.h"
#include "TopExceptionScope.h"
#include <mutex>
#include <numbers>
#include <wtf/MathExtras.h>
#include <wtf/NeverDestroyed.h>

namespace JSC { namespace AOT { namespace Typed {

unsigned Builtin::numberOfRequiredParameters() const
{
    unsigned result = 0;
    for (auto& parameter : parameters)
        result += parameter.mode == ParameterMode::Required;
    return result;
}

bool Builtin::takes(unsigned numberOfArguments) const
{
    if (numberOfArguments < numberOfRequiredParameters())
        return false;
    return numberOfArguments <= parameters.size() || (!parameters.isEmpty() && parameters.last().mode == ParameterMode::Rest);
}

static uint64_t keyFor(BuiltinKind kind, BuiltinName holder, BuiltinName name)
{
    return static_cast<uint64_t>(kind) << 32 | static_cast<uint64_t>(holder.index()) << 16 | name.index();
}

// Either.
static BuiltinResult operator|(BuiltinResult a, BuiltinResult b)
{
    RELEASE_ASSERT(!a.isAnything() && !b.isAnything());
    // There is room for one range and for one class.
    RELEASE_ASSERT(!(a.kinds & b.kinds & (BuiltinResult::Integer | BuiltinResult::Instance)));
    if (b.kinds & BuiltinResult::Integer) {
        a.least = b.least;
        a.most = b.most;
    }
    if (b.kinds & BuiltinResult::Instance)
        a.engineClass = b.engineClass;
    a.kinds |= b.kinds;
    a.arguments |= b.arguments;
    return a;
}

class BuiltinsFiller {
public:
    explicit BuiltinsFiller(Builtins& table)
        : m_table(table)
    {
    }

    void fill();

private:
    // Before the results below, which add classes to it.
    Builtins& m_table;

    using Parameters = std::initializer_list<BuiltinParameter>;

    static constexpr uint16_t Pure = Effect::Pure;
    static constexpr uint16_t Throws = Effect::Throws;
    static constexpr uint16_t ReadsArguments = Effect::ReadsArguments;
    static constexpr uint16_t WritesArguments = Effect::WritesArguments;
    static constexpr uint16_t ReadsWorld = Effect::ReadsWorld;
    static constexpr uint16_t Unknown = Effect::Unknown;
    static constexpr uint16_t Reads = Effect::ReadsThis;
    static constexpr uint16_t Writes = Effect::ReadsThis | Effect::WritesThis;
    static constexpr uint16_t Makes = Effect::Allocates;
    static constexpr uint16_t Copies = Effect::ReadsThis | Effect::Allocates;
    static constexpr uint16_t World = Effect::ReadsWorld | Effect::WritesWorld;

    static constexpr BuiltinParameter asIs { ArgumentConversion::None };
    static constexpr BuiltinParameter isNumber { ArgumentConversion::RequiresNumber };
    static constexpr BuiltinParameter isTypedArray { ArgumentConversion::RequiresTypedArray };
    static constexpr BuiltinParameter toNumber { ArgumentConversion::ToNumber };
    static constexpr BuiltinParameter toString { ArgumentConversion::ToString };
    static constexpr BuiltinParameter toPropertyKey { ArgumentConversion::ToPropertyKey };
    static constexpr BuiltinParameter optional(BuiltinParameter parameter) { return { parameter.conversion, ParameterMode::Optional }; }
    static constexpr BuiltinParameter rest(BuiltinParameter parameter) { return { parameter.conversion, ParameterMode::Rest }; }

    static constexpr BuiltinResult of(uint16_t kinds)
    {
        BuiltinResult result;
        result.kinds = kinds;
        return result;
    }
    static constexpr BuiltinResult integer(int64_t least, int64_t most)
    {
        BuiltinResult result = of(BuiltinResult::Integer);
        result.least = least;
        result.most = most;
        return result;
    }
    static constexpr BuiltinResult argument(unsigned index)
    {
        BuiltinResult result;
        result.arguments = 1 << index;
        return result;
    }
    BuiltinResult fresh(ASCIILiteral className)
    {
        BuiltinResult result = of(BuiltinResult::Instance);
        result.engineClass = engineClass(className, className, NotTypedArray);
        return result;
    }

    // The engine vouches for nothing: it is what code of the program has made, or what a container holds.
    const BuiltinResult anything { };
    const BuiltinResult undefined = of(BuiltinResult::Undefined);
    const BuiltinResult null = of(BuiltinResult::Null);
    const BuiltinResult boolean = of(BuiltinResult::Boolean);
    const BuiltinResult number = of(BuiltinResult::Number);
    const BuiltinResult string = of(BuiltinResult::String);
    const BuiltinResult symbol = of(BuiltinResult::Symbol);
    const BuiltinResult bigInt = of(BuiltinResult::BigInt);
    const BuiltinResult thisValue = of(BuiltinResult::This);
    const BuiltinResult sameClass = of(BuiltinResult::InstanceOfSameClass);
    const BuiltinResult int32 = integer(INT32_MIN, INT32_MAX);
    const BuiltinResult uint32 = integer(0, UINT32_MAX);
    const BuiltinResult stringLength = integer(0, INT32_MAX);
    const BuiltinResult placeInStringOrMinusOne = integer(-1, INT32_MAX);
    const BuiltinResult arrayLength = uint32;
    const BuiltinResult placeInArrayOrMinusOne = integer(-1, UINT32_MAX);
    const BuiltinResult bufferLength = integer(0, MAX_ARRAY_BUFFER_SIZE);
    const BuiltinResult placeInBufferOrMinusOne = integer(-1, MAX_ARRAY_BUFFER_SIZE);
    const BuiltinResult sizeOfMapOrSet = integer(0, INT32_MAX);
    const BuiltinResult stringOrUndefined = string | undefined;
    const BuiltinResult newArray = fresh("Array"_s);
    const BuiltinResult newPromise = fresh("Promise"_s);
    // Some object: no type tells the instances of Object from other objects.
    const BuiltinResult object = fresh("Object"_s);

    static constexpr BuiltinImplementation engine { };
    static constexpr BuiltinImplementation inlined(InlineBuiltin which) { return { BuiltinImplementation::Inline, which, 0 }; }
    static constexpr BuiltinImplementation is(double number) { return { BuiltinImplementation::Number, InlineBuiltin::This, number }; }
    static constexpr BuiltinImplementation does(LanguageOperation which) { return { BuiltinImplementation::Operation, InlineBuiltin::This, 0, which }; }

    void add(BuiltinKind kind, ASCIILiteral holder, ASCIILiteral name, Parameters parameters, const BuiltinResult& result, uint16_t effects, BuiltinImplementation implementation)
    {
        BuiltinName holderName = holder.isEmpty() ? BuiltinName() : intern(holder);
        BuiltinName nameName = intern(name);
        m_table.m_first.add(keyFor(kind, holderName, nameName), static_cast<uint16_t>(m_table.m_entries.size()));
        if (kind == BuiltinKind::Function || kind == BuiltinKind::Constructor)
            m_table.m_isGlobal[nameName.index()] = true;
        else if (kind == BuiltinKind::StaticFunction || kind == BuiltinKind::Constant)
            m_table.m_isGlobal[holderName.index()] = true;
        else if (kind != BuiltinKind::Operation)
            engineClass(holder, holder, NotTypedArray);
        m_table.m_entries.append(Builtin { kind, holder, name, holderName, nameName, parameters, result, effects, implementation });
    }

    BuiltinName intern(ASCIILiteral text)
    {
        auto result = m_table.m_names.add(String(text), static_cast<uint16_t>(m_table.m_names.size()));
        if (result.isNewEntry) {
            m_table.m_texts.append(text);
            m_table.m_isGlobal.append(false);
            m_table.m_mayBeExtended.append(false);
            m_table.m_classOfName.append(UINT16_MAX);
        }
        return BuiltinName(result.iterator->value);
    }

    uint16_t engineClass(ASCIILiteral text, ASCIILiteral listedAs, TypedArrayType typedArrayType)
    {
        BuiltinName name = intern(text);
        BuiltinName listedAsName = intern(listedAs);
        uint16_t& index = m_table.m_classOfName[name.index()];
        if (index == UINT16_MAX) {
            index = m_table.m_classes.size();
            m_table.m_classes.append(EngineClass { text, index, name, listedAsName, typedArrayType, typedArrayType == NotTypedArray ? CellType : typeForTypedArrayType(typedArrayType) });
        }
        return index;
    }

    void engineClass(ASCIILiteral text, JSType type)
    {
        m_table.m_classes[engineClass(text, text, NotTypedArray)].type = type;
    }

    void function(ASCIILiteral name, Parameters parameters, const BuiltinResult& result, uint16_t effects, BuiltinImplementation implementation = engine) { add(BuiltinKind::Function, ""_s, name, parameters, result, effects, implementation); }
    void constructor(ASCIILiteral name, Parameters parameters, const BuiltinResult& result, uint16_t effects, BuiltinImplementation implementation = engine) { add(BuiltinKind::Constructor, ""_s, name, parameters, result, effects, implementation); }
    void staticFunction(ASCIILiteral holder, ASCIILiteral name, Parameters parameters, const BuiltinResult& result, uint16_t effects, BuiltinImplementation implementation = engine) { add(BuiltinKind::StaticFunction, holder, name, parameters, result, effects, implementation); }
    void method(ASCIILiteral holder, ASCIILiteral name, Parameters parameters, const BuiltinResult& result, uint16_t effects, BuiltinImplementation implementation = engine) { add(BuiltinKind::Method, holder, name, parameters, result, effects, implementation); }
    void getter(ASCIILiteral holder, ASCIILiteral name, const BuiltinResult& result, uint16_t effects, BuiltinImplementation implementation = engine) { add(BuiltinKind::Getter, holder, name, { }, result, effects, implementation); }
    void setter(ASCIILiteral holder, ASCIILiteral name, BuiltinParameter parameter, uint16_t effects, BuiltinImplementation implementation) { add(BuiltinKind::Setter, holder, name, { parameter }, undefined, effects, implementation); }
    void constant(ASCIILiteral holder, ASCIILiteral name, double value) { add(BuiltinKind::Constant, holder, name, { }, number, Pure, is(value)); }
    void operation(ASCIILiteral name, Parameters parameters, const BuiltinResult& result, uint16_t effects, LanguageOperation which) { add(BuiltinKind::Operation, ""_s, name, parameters, result, effects, does(which)); }
    // Of these there is nothing to know. They are listed so that the table has all there is to each object it talks about.
    void methodsThatSayNothing(ASCIILiteral holder, std::initializer_list<ASCIILiteral> names)
    {
        for (auto name : names)
            method(holder, name, { rest(asIs) }, anything, Unknown);
    }
    void staticFunctionsThatSayNothing(ASCIILiteral holder, std::initializer_list<ASCIILiteral> names)
    {
        for (auto name : names)
            staticFunction(holder, name, { rest(asIs) }, anything, Unknown);
    }

    void fillOperations();
    void fillHostObjects();
    void fillFundamentalObjects();
    void fillNumbersAndDates();
    void fillText();
    void fillIndexedCollections();
    void fillKeyedCollections();
    void fillControlAbstractions();
};

void BuiltinsFiller::fillOperations()
{
    using enum LanguageOperation;
    // `{ ...source }`: into the object, which is handed back.
    operation("CopyDataProperties"_s, { asIs, asIs }, argument(0), Unknown, CopyDataProperties);
    // `{ a, ...rest } = source`: the source, then the keys that are left out.
    operation("CopyDataPropertiesExcluding"_s, { asIs, rest(asIs) }, object, Unknown, CopyDataPropertiesExcluding);
    // `{ [key]: value }`.
    operation("DefineField"_s, { asIs, asIs, asIs }, undefined, Unknown, DefineField);
    // `{ get key() {} }`: the object, the key, the function, whether it shows when the properties are listed.
    operation("DefineGetter"_s, { asIs, asIs, asIs, asIs }, undefined, Unknown, DefineGetter);
    operation("DefineSetter"_s, { asIs, asIs, asIs, asIs }, undefined, Unknown, DefineSetter);
    // The keys `for in` goes through.
    operation("ForInKeys"_s, { asIs }, newArray, Unknown, ForInKeys);
    // Whether `for in` still comes to the key: it has not been deleted, nor hidden by a property that does not show. The value, the key.
    operation("HasEnumerableProperty"_s, { asIs, asIs }, boolean, Unknown, HasEnumerableProperty);
    // `[...iterable]`: onto the array, which compiled code has made and is handed back.
    operation("AppendSpread"_s, { asIs, asIs }, argument(0), WritesArguments | ReadsArguments | Throws | Makes, AppendSpread);
    // `[, ]`.
    operation("AppendHole"_s, { asIs }, argument(0), WritesArguments | Throws | Makes, AppendHole);
    // `callee.call(thisValue, ...array)` and `new callee(...array)`.
    operation("Apply"_s, { asIs, asIs, asIs }, anything, Unknown, Apply);
    operation("ConstructWithArray"_s, { asIs, asIs }, object, Unknown, ConstructWithArray);
    // `new callee(...)` of what is no class, with the object already made, of the layout of the type of the expression: the callee, the
    // object, the arguments. An object that the callee returns takes its place.
    operation("ConstructWith"_s, { asIs, asIs, rest(asIs) }, object, Unknown, ConstructWith);
    // The same with the arguments in an array.
    operation("ConstructWithSpread"_s, { asIs, asIs, asIs }, object, Unknown, ConstructWithSpread);
    // At the declaration of a class that is not evaluated once or extends something foreign. The closure of the constructor, which
    // becomes the class object and is handed back, and what stands after `extends`, which is asked for its `prototype`: `undefined` if
    // nothing does, or a class of the program that is evaluated once.
    operation("MakeClass"_s, { asIs, asIs }, argument(0), Unknown, MakeClass);
    // `new C()` of a class that is laid out and not evaluated once: an object of the class, with the prototype of that class object.
    operation("NewObjectOfClass"_s, { asIs }, object, Makes, NewObjectOfClass);
    // `#name() {}` of a class that is not laid out: the object nobody sees that holds them, the key, the function.
    operation("DefineMethod"_s, { asIs, asIs, asIs }, undefined, Unknown, DefineMethod);
    // `super.key`. The class object of the class around the code, the key, `this`, whether the code is static: the search starts behind
    // the class object then, else behind its `prototype`. resolveCalls() carries it out where the classes say what is found.
    operation("SuperGet"_s, { asIs, asIs, asIs, asIs }, anything, Unknown, SuperGet);
    // `super.key = value`. The class object, the key, the value, `this`, whether the code is static.
    operation("SuperSet"_s, { asIs, asIs, asIs, asIs, asIs }, undefined, Unknown, SuperSet);
    // `super(...)` of a class that extends something foreign. The class, `new.target`, the arguments as an array: yields `this`.
    operation("SuperConstruct"_s, { asIs, asIs, asIs }, object, Unknown, SuperConstruct);
    // `super(...)` where the constructor of the class that is extended cannot be called by name. That class, `this`, `new.target`, the
    // arguments as an array: yields what the constructor returns.
    operation("ConstructOn"_s, { asIs, asIs, asIs, asIs }, anything, Unknown, ConstructOn);
    // What `return` was given, `this`.
    operation("ConstructorResult"_s, { asIs, asIs }, argument(0) | argument(1), Throws, ConstructorResult);
    operation("NewPrivateName"_s, { asIs }, symbol, Makes, NewPrivateName);
    // `#name` of a class that is not laid out: the object, the key that was made for the name, the value.
    operation("PrivateGet"_s, { asIs, asIs }, anything, Unknown, PrivateGet);
    operation("PrivateSet"_s, { asIs, asIs, asIs }, undefined, Unknown, PrivateSet);
    operation("PrivateHas"_s, { asIs, asIs }, boolean, Unknown, PrivateHas);
    operation("PrivateFieldAdd"_s, { asIs, asIs, asIs }, undefined, Unknown, PrivateFieldAdd);
    // The new object, and an object nobody sees that has what the class declares as `#name() {}`, `get #name() {}`, `set #name(v) {}`.
    operation("PrivateMethodsAdd"_s, { asIs, asIs }, undefined, Unknown, PrivateMethodsAdd);
    // Async functions, see AOTTypedCoroutineSplitting.cpp. What a promise is resolved with is asked for its `then`.
    operation("NewPromise"_s, { }, newPromise, Makes, NewPromise);
    operation("ResolvePromise"_s, { asIs, asIs }, undefined, ReadsArguments | World | Throws, ResolvePromise);
    operation("RejectPromise"_s, { asIs, asIs }, undefined, World, RejectPromise);
    // The resume closure of a generator. next(), throw() and return() store the value they are given to the frame and call the closure with the resume mode.
    operation("NewGenerator"_s, { asIs }, fresh("Generator"_s), Makes, NewGenerator);
    operation("NewAsyncGenerator"_s, { asIs }, fresh("AsyncGenerator"_s), Makes, NewAsyncGenerator);
    operation("GetAsyncIterator"_s, { asIs }, anything, Unknown, GetAsyncIterator);
    // The arguments as an array, which compiled code has made: yields the (unmapped) arguments object.
    operation("CreateArguments"_s, { asIs }, object, Makes, CreateArguments);
    operation("GetIterator"_s, { asIs }, anything, Unknown, GetIterator);
    operation("RequireIteratorResultObject"_s, { asIs }, undefined, Throws, RequireIteratorResultObject);
    operation("IteratorCloseAndThrowMissingThrowMethod"_s, { asIs }, undefined, Unknown, IteratorCloseAndThrowMissingThrowMethod);
    // `using` declarations. The stack is an array of value and method pairs, with the pending error in two properties.
    operation("NewDisposableStack"_s, { }, anything, Makes, NewDisposableStack);
    // The stack, the resource, whether the declaration is `await using`.
    operation("AddDisposableResource"_s, { asIs, asIs, asIs }, undefined, Unknown, AddDisposableResource);
    // Disposes of the last resource. Yields the stack if it is empty, null if there is nothing to await, else the value to await.
    operation("DisposeNextResource"_s, { asIs }, anything, Unknown, DisposeNextResource);
    operation("RecordDisposeError"_s, { asIs, asIs }, undefined, Unknown, RecordDisposeError);
    // The stack, whether the block completed by throwing, the completion value. Disposes of what is left and throws the pending error.
    operation("DisposeResources"_s, { asIs, asIs, asIs }, undefined, Unknown, DisposeResources);
    // What an ES module sees of `module.exports`: `default`, and a getter for each own property. The second argument: whether
    // `default` is `module.exports` even if that has `__esModule`.
    operation("ToESM"_s, { asIs, asIs }, anything, Unknown, ToESM);
    operation("ParseJSON"_s, { asIs }, anything, Makes | Throws, ParseJSON);
    operation("RequireExternalModule"_s, { asIs }, anything, Unknown, RequireExternalModule);
    operation("GetRequireFunction"_s, { }, anything, Makes | Throws, GetRequireFunction);
    // The cooked strings and the raw strings of a tagged template, as arrays: yields the frozen template object.
    operation("GetTemplateObject"_s, { asIs, asIs }, anything, Unknown, GetTemplateObject);
    operation("GetImportMeta"_s, { }, anything, Makes | Throws, GetImportMeta);
    // `import(specifier)` of a module that is not compiled with the program.
    operation("ImportExternalModule"_s, { asIs }, anything, Unknown, ImportExternalModule);
    // The resume closure, the awaited value, which is asked for its `constructor` and its `then`. The closure is called from a microtask.
    operation("Await"_s, { asIs, asIs }, undefined, ReadsArguments | World | Throws, Await);
}

void BuiltinsFiller::fillHostObjects()
{
    for (auto name : { "log"_s, "info"_s, "warn"_s, "error"_s, "debug"_s, "trace"_s, "dir"_s, "dirxml"_s, "table"_s, "assert"_s, "group"_s, "groupCollapsed"_s })
        staticFunction("console"_s, name, { rest(asIs) }, undefined, ReadsArguments | World | Throws);
    for (auto name : { "groupEnd"_s, "clear"_s })
        staticFunction("console"_s, name, { }, undefined, World);
    for (auto name : { "count"_s, "countReset"_s, "time"_s, "timeEnd"_s, "profile"_s, "profileEnd"_s, "timeStamp"_s })
        staticFunction("console"_s, name, { optional(toString) }, undefined, World | Throws);
    staticFunction("console"_s, "timeLog"_s, { optional(toString), rest(asIs) }, undefined, ReadsArguments | World | Throws);
    m_table.m_mayBeExtended[intern("console"_s).index()] = true;
    // Buffer, which has methods of its own under the names of some of those it inherits.
    m_table.m_classes[engineClass("Uint8Array"_s, "TypedArray"_s, TypeUint8)].isExtendedByHost = true;
}

void BuiltinsFiller::fillFundamentalObjects()
{
    // The global object.
    function("isNaN"_s, { toNumber }, boolean, Pure, inlined(InlineBuiltin::IsNaN));
    function("isFinite"_s, { toNumber }, boolean, Pure, inlined(InlineBuiltin::IsFinite));
    function("parseFloat"_s, { toString }, number, Pure);
    function("parseInt"_s, { toString, optional(toNumber) }, number, Pure);
    for (auto name : { "encodeURI"_s, "encodeURIComponent"_s, "decodeURI"_s, "decodeURIComponent"_s })
        function(name, { toString }, string, Throws);
    function("escape"_s, { toString }, string, Pure);
    function("unescape"_s, { toString }, string, Pure);
    function("eval"_s, { rest(asIs) }, anything, Unknown);

    // Object.
    function("Object"_s, { rest(asIs) }, object, Unknown);
    constructor("Object"_s, { rest(asIs) }, object, Unknown);
    // What is no object is handed back as it is, or refused.
    for (auto name : { "defineProperty"_s, "defineProperties"_s, "freeze"_s, "preventExtensions"_s, "seal"_s, "setPrototypeOf"_s })
        staticFunction("Object"_s, name, { asIs, rest(asIs) }, argument(0), Unknown);
    for (auto name : { "create"_s, "fromEntries"_s, "getOwnPropertyDescriptors"_s, "groupBy"_s })
        staticFunction("Object"_s, name, { rest(asIs) }, object, Unknown);
    for (auto name : { "entries"_s, "getOwnPropertyNames"_s, "getOwnPropertySymbols"_s, "keys"_s, "values"_s })
        staticFunction("Object"_s, name, { asIs }, newArray, ReadsArguments | Makes | Throws);
    for (auto name : { "isExtensible"_s, "isFrozen"_s, "isSealed"_s })
        staticFunction("Object"_s, name, { asIs }, boolean, ReadsArguments);
    // Of a primitive value it makes an object.
    staticFunctionsThatSayNothing("Object"_s, { "assign"_s });
    staticFunction("Object"_s, "getOwnPropertyDescriptor"_s, { rest(asIs) }, object | undefined, Unknown);
    staticFunction("Object"_s, "getPrototypeOf"_s, { asIs }, object | null, ReadsArguments | Throws);
    staticFunction("Object"_s, "hasOwn"_s, { asIs, toPropertyKey }, boolean, ReadsArguments | Throws);
    staticFunction("Object"_s, "is"_s, { asIs, asIs }, boolean, Pure);
    method("Object"_s, "hasOwnProperty"_s, { toPropertyKey }, boolean, Reads);
    method("Object"_s, "isPrototypeOf"_s, { asIs }, boolean, ReadsArguments);
    method("Object"_s, "propertyIsEnumerable"_s, { toPropertyKey }, boolean, Reads);
    method("Object"_s, "toString"_s, { }, string, Reads);
    method("Object"_s, "valueOf"_s, { }, thisValue, Pure, inlined(InlineBuiltin::This));
    method("Object"_s, "__defineGetter__"_s, { rest(asIs) }, undefined, Unknown);
    method("Object"_s, "__defineSetter__"_s, { rest(asIs) }, undefined, Unknown);
    methodsThatSayNothing("Object"_s, { "toLocaleString"_s, "__lookupGetter__"_s, "__lookupSetter__"_s });

    // Function.
    function("Function"_s, { rest(asIs) }, anything, Unknown);
    constructor("Function"_s, { rest(asIs) }, anything, Unknown);
    methodsThatSayNothing("Function"_s, { "apply"_s, "bind"_s, "call"_s });
    method("Function"_s, "toString"_s, { }, string, Pure);
    method("Function"_s, "@@hasInstance"_s, { asIs }, boolean, ReadsArguments);

    // Boolean.
    function("Boolean"_s, { optional(asIs) }, boolean, Pure);
    constructor("Boolean"_s, { optional(asIs) }, fresh("Boolean"_s), Makes);
    method("Boolean"_s, "toString"_s, { }, string, Pure);
    method("Boolean"_s, "valueOf"_s, { }, boolean, Pure);

    // Symbol.
    function("Symbol"_s, { optional(toString) }, symbol, Makes);
    staticFunction("Symbol"_s, "for"_s, { toString }, symbol, World);
    staticFunction("Symbol"_s, "keyFor"_s, { asIs }, stringOrUndefined, ReadsWorld | Throws);
    for (auto name : { "asyncDispose"_s, "asyncIterator"_s, "dispose"_s, "hasInstance"_s, "isConcatSpreadable"_s, "iterator"_s, "match"_s, "matchAll"_s, "replace"_s, "search"_s, "species"_s, "split"_s, "toPrimitive"_s, "toStringTag"_s, "unscopables"_s })
        add(BuiltinKind::Constant, "Symbol"_s, name, { }, symbol, Pure, engine);
    method("Symbol"_s, "toString"_s, { }, string, Pure);
    method("Symbol"_s, "valueOf"_s, { }, symbol, Pure, inlined(InlineBuiltin::This));
    method("Symbol"_s, "@@toPrimitive"_s, { asIs }, symbol, Pure, inlined(InlineBuiltin::This));

    // Errors.
    const BuiltinResult newError = fresh("Error"_s);
    function("Error"_s, { rest(asIs) }, newError, Unknown);
    constructor("Error"_s, { optional(toString) }, newError, Makes | ReadsWorld);
    constructor("Error"_s, { rest(asIs) }, newError, Unknown);
    for (auto name : { "EvalError"_s, "RangeError"_s, "ReferenceError"_s, "SyntaxError"_s, "TypeError"_s, "URIError"_s })
        constructor(name, { optional(toString) }, newError, Makes | ReadsWorld);
    constructor("AggregateError"_s, { rest(asIs) }, newError, Unknown);
    constructor("SuppressedError"_s, { rest(asIs) }, newError, Unknown);
    // A host puts its own in the place of the engine's when the realm has been made, as a property like any other.
    staticFunctionsThatSayNothing("Error"_s, { "captureStackTrace"_s });
    staticFunction("Error"_s, "isError"_s, { asIs }, boolean, Pure);
    // Every program extends one of these classes, and their objects all have the type of those of Error.
    methodsThatSayNothing("Error"_s, { "toString"_s });
}

void BuiltinsFiller::fillNumbersAndDates()
{
    // Number.
    function("Number"_s, { optional(toNumber) }, number, Pure, inlined(InlineBuiltin::ToNumber));
    constructor("Number"_s, { optional(toNumber) }, fresh("Number"_s), Makes);
    constant("Number"_s, "EPSILON"_s, std::numeric_limits<double>::epsilon());
    constant("Number"_s, "MAX_SAFE_INTEGER"_s, 9007199254740991.0);
    constant("Number"_s, "MAX_VALUE"_s, std::numeric_limits<double>::max());
    constant("Number"_s, "MIN_SAFE_INTEGER"_s, -9007199254740991.0);
    constant("Number"_s, "MIN_VALUE"_s, 5e-324);
    constant("Number"_s, "NaN"_s, PNaN);
    constant("Number"_s, "NEGATIVE_INFINITY"_s, -std::numeric_limits<double>::infinity());
    constant("Number"_s, "POSITIVE_INFINITY"_s, std::numeric_limits<double>::infinity());
    staticFunction("Number"_s, "isFinite"_s, { asIs }, boolean, Pure, inlined(InlineBuiltin::NumberIsFinite));
    staticFunction("Number"_s, "isInteger"_s, { asIs }, boolean, Pure, inlined(InlineBuiltin::NumberIsInteger));
    staticFunction("Number"_s, "isNaN"_s, { asIs }, boolean, Pure, inlined(InlineBuiltin::NumberIsNaN));
    staticFunction("Number"_s, "isSafeInteger"_s, { asIs }, boolean, Pure, inlined(InlineBuiltin::NumberIsSafeInteger));
    staticFunction("Number"_s, "parseFloat"_s, { toString }, number, Pure);
    staticFunction("Number"_s, "parseInt"_s, { toString, optional(toNumber) }, number, Pure);
    for (auto name : { "toExponential"_s, "toFixed"_s, "toPrecision"_s, "toString"_s })
        method("Number"_s, name, { optional(toNumber) }, string, Throws);
    method("Number"_s, "toLocaleString"_s, { rest(asIs) }, string, Unknown);
    method("Number"_s, "valueOf"_s, { }, number, Pure, inlined(InlineBuiltin::This));

    // BigInt.
    function("BigInt"_s, { asIs }, bigInt, ReadsArguments | Throws);
    staticFunction("BigInt"_s, "asIntN"_s, { toNumber, asIs }, bigInt, ReadsArguments | Throws);
    staticFunction("BigInt"_s, "asUintN"_s, { toNumber, asIs }, bigInt, ReadsArguments | Throws);
    method("BigInt"_s, "toLocaleString"_s, { rest(asIs) }, string, Unknown);
    method("BigInt"_s, "toString"_s, { optional(toNumber) }, string, Throws);
    method("BigInt"_s, "valueOf"_s, { }, bigInt, Pure, inlined(InlineBuiltin::This));

    // Math.
    constant("Math"_s, "E"_s, Math::exp(1.0));
    constant("Math"_s, "LN10"_s, Math::log(10.0));
    constant("Math"_s, "LN2"_s, Math::log(2.0));
    constant("Math"_s, "LOG10E"_s, Math::log10(Math::exp(1.0)));
    constant("Math"_s, "LOG2E"_s, Math::log2(Math::exp(1.0)));
    constant("Math"_s, "PI"_s, std::numbers::pi);
    constant("Math"_s, "SQRT1_2"_s, sqrt(0.5));
    constant("Math"_s, "SQRT2"_s, sqrt(2.0));
    for (auto name : { "acos"_s, "acosh"_s, "asin"_s, "asinh"_s, "atan"_s, "atanh"_s, "cbrt"_s, "cos"_s, "cosh"_s, "exp"_s, "expm1"_s, "fround"_s, "f16round"_s, "log"_s, "log1p"_s, "log10"_s, "log2"_s, "round"_s, "sign"_s, "sin"_s, "sinh"_s, "tan"_s, "tanh"_s })
        staticFunction("Math"_s, name, { toNumber }, number, Pure);
    staticFunction("Math"_s, "abs"_s, { toNumber }, number, Pure, inlined(InlineBuiltin::MathAbs));
    staticFunction("Math"_s, "atan2"_s, { toNumber, toNumber }, number, Pure);
    staticFunction("Math"_s, "ceil"_s, { toNumber }, number, Pure, inlined(InlineBuiltin::MathCeil));
    staticFunction("Math"_s, "clz32"_s, { toNumber }, integer(0, 32), Pure, inlined(InlineBuiltin::MathClz32));
    staticFunction("Math"_s, "floor"_s, { toNumber }, number, Pure, inlined(InlineBuiltin::MathFloor));
    staticFunction("Math"_s, "hypot"_s, { rest(toNumber) }, number, Pure);
    staticFunction("Math"_s, "imul"_s, { toNumber, toNumber }, int32, Pure, inlined(InlineBuiltin::MathImul));
    staticFunction("Math"_s, "max"_s, { rest(toNumber) }, number, Pure, inlined(InlineBuiltin::MathMax));
    staticFunction("Math"_s, "min"_s, { rest(toNumber) }, number, Pure, inlined(InlineBuiltin::MathMin));
    staticFunction("Math"_s, "pow"_s, { toNumber, toNumber }, number, Pure, inlined(InlineBuiltin::MathPow));
    staticFunction("Math"_s, "random"_s, { }, number, World);
    staticFunction("Math"_s, "sqrt"_s, { toNumber }, number, Pure, inlined(InlineBuiltin::MathSqrt));
    staticFunction("Math"_s, "sumPrecise"_s, { rest(asIs) }, number, Unknown);
    staticFunction("Math"_s, "trunc"_s, { toNumber }, number, Pure, inlined(InlineBuiltin::MathTrunc));

    // Date. The time it stands for only the engine looks at.
    const BuiltinResult newDate = fresh("Date"_s);
    function("Date"_s, { rest(asIs) }, string, ReadsWorld);
    constructor("Date"_s, { }, newDate, Makes | ReadsWorld);
    constructor("Date"_s, { asIs }, newDate, Makes | ReadsWorld | ReadsArguments | Throws);
    constructor("Date"_s, { toNumber, toNumber, optional(toNumber), optional(toNumber), optional(toNumber), optional(toNumber), optional(toNumber) }, newDate, Makes | ReadsWorld);
    staticFunction("Date"_s, "now"_s, { }, number, ReadsWorld);
    staticFunction("Date"_s, "parse"_s, { toString }, number, ReadsWorld);
    staticFunction("Date"_s, "UTC"_s, { toNumber, optional(toNumber), optional(toNumber), optional(toNumber), optional(toNumber), optional(toNumber), optional(toNumber) }, number, Pure);
    for (auto name : { "getDate"_s, "getDay"_s, "getFullYear"_s, "getHours"_s, "getMilliseconds"_s, "getMinutes"_s, "getMonth"_s, "getSeconds"_s, "getTime"_s, "getTimezoneOffset"_s, "getUTCDate"_s, "getUTCDay"_s, "getUTCFullYear"_s, "getUTCHours"_s, "getUTCMilliseconds"_s, "getUTCMinutes"_s, "getUTCMonth"_s, "getUTCSeconds"_s, "getYear"_s, "valueOf"_s })
        method("Date"_s, name, { }, number, ReadsWorld);
    for (auto name : { "setDate"_s, "setMilliseconds"_s, "setTime"_s, "setUTCDate"_s, "setUTCMilliseconds"_s, "setYear"_s })
        method("Date"_s, name, { toNumber }, number, World);
    for (auto name : { "setMonth"_s, "setSeconds"_s, "setUTCMonth"_s, "setUTCSeconds"_s })
        method("Date"_s, name, { toNumber, optional(toNumber) }, number, World);
    for (auto name : { "setFullYear"_s, "setMinutes"_s, "setUTCFullYear"_s, "setUTCMinutes"_s })
        method("Date"_s, name, { toNumber, optional(toNumber), optional(toNumber) }, number, World);
    for (auto name : { "setHours"_s, "setUTCHours"_s })
        method("Date"_s, name, { toNumber, optional(toNumber), optional(toNumber), optional(toNumber) }, number, World);
    for (auto name : { "toDateString"_s, "toGMTString"_s, "toString"_s, "toTimeString"_s, "toUTCString"_s })
        method("Date"_s, name, { }, string, ReadsWorld);
    method("Date"_s, "toISOString"_s, { }, string, ReadsWorld | Throws);
    for (auto name : { "toLocaleDateString"_s, "toLocaleString"_s, "toLocaleTimeString"_s })
        method("Date"_s, name, { rest(asIs) }, string, Unknown);
    // They call other methods by name.
    methodsThatSayNothing("Date"_s, { "toJSON"_s, "toTemporalInstant"_s, "@@toPrimitive"_s });
}

void BuiltinsFiller::fillText()
{
    // String. What takes a regular expression asks it for a method: only of a primitive value is it known that it has none.
    function("String"_s, { optional(asIs) }, string, ReadsArguments);
    constructor("String"_s, { optional(toString) }, fresh("String"_s), Makes);
    staticFunction("String"_s, "fromCharCode"_s, { rest(toNumber) }, string, Pure);
    staticFunction("String"_s, "fromCodePoint"_s, { rest(toNumber) }, string, Throws);
    staticFunction("String"_s, "raw"_s, { rest(asIs) }, string, Unknown);
    getter("String"_s, "length"_s, stringLength, Pure, inlined(InlineBuiltin::StringLength));
    method("String"_s, "at"_s, { toNumber }, stringOrUndefined, Pure, inlined(InlineBuiltin::StringAt));
    method("String"_s, "charAt"_s, { toNumber }, string, Pure, inlined(InlineBuiltin::StringCharAt));
    method("String"_s, "charCodeAt"_s, { toNumber }, number, Pure, inlined(InlineBuiltin::StringCharCodeAt));
    method("String"_s, "codePointAt"_s, { toNumber }, integer(0, 0x10ffff) | undefined, Pure, inlined(InlineBuiltin::StringCodePointAt));
    method("String"_s, "concat"_s, { rest(toString) }, string, Throws);
    method("String"_s, "endsWith"_s, { toString, optional(toNumber) }, boolean, Pure, inlined(InlineBuiltin::StringEndsWith));
    method("String"_s, "includes"_s, { toString, optional(toNumber) }, boolean, Pure, inlined(InlineBuiltin::StringIncludes));
    method("String"_s, "startsWith"_s, { toString, optional(toNumber) }, boolean, Pure, inlined(InlineBuiltin::StringStartsWith));
    method("String"_s, "indexOf"_s, { toString, optional(toNumber) }, placeInStringOrMinusOne, Pure, inlined(InlineBuiltin::StringIndexOf));
    method("String"_s, "lastIndexOf"_s, { toString, optional(toNumber) }, placeInStringOrMinusOne, Pure);
    method("String"_s, "isWellFormed"_s, { }, boolean, Pure);
    method("String"_s, "localeCompare"_s, { toString }, number, ReadsWorld);
    method("String"_s, "localeCompare"_s, { rest(asIs) }, number, Unknown);
    method("String"_s, "match"_s, { toString }, newArray | null, Makes | World | Throws);
    method("String"_s, "match"_s, { rest(asIs) }, anything, Unknown);
    method("String"_s, "normalize"_s, { optional(toString) }, string, Throws);
    method("String"_s, "padEnd"_s, { toNumber, optional(toString) }, string, Throws);
    method("String"_s, "padStart"_s, { toNumber, optional(toString) }, string, Throws);
    method("String"_s, "repeat"_s, { toNumber }, string, Throws);
    method("String"_s, "replace"_s, { toString, toString }, string, Throws);
    method("String"_s, "replace"_s, { rest(asIs) }, anything, Unknown);
    method("String"_s, "replaceAll"_s, { toString, toString }, string, Throws);
    method("String"_s, "replaceAll"_s, { rest(asIs) }, anything, Unknown);
    method("String"_s, "search"_s, { toString }, placeInStringOrMinusOne, Makes | World | Throws);
    method("String"_s, "search"_s, { rest(asIs) }, anything, Unknown);
    method("String"_s, "slice"_s, { optional(toNumber), optional(toNumber) }, string, Pure, inlined(InlineBuiltin::StringSlice));
    method("String"_s, "split"_s, { optional(toString), optional(toNumber) }, newArray, Makes | Throws);
    method("String"_s, "split"_s, { rest(asIs) }, anything, Unknown);
    method("String"_s, "substr"_s, { optional(toNumber), optional(toNumber) }, string, Pure, inlined(InlineBuiltin::StringSubstr));
    method("String"_s, "substring"_s, { optional(toNumber), optional(toNumber) }, string, Pure, inlined(InlineBuiltin::StringSubstring));
    for (auto name : { "toLocaleLowerCase"_s, "toLocaleUpperCase"_s }) {
        method("String"_s, name, { }, string, ReadsWorld);
        method("String"_s, name, { rest(asIs) }, string, Unknown);
    }
    for (auto name : { "toLowerCase"_s, "toUpperCase"_s, "toWellFormed"_s, "trim"_s, "trimEnd"_s, "trimLeft"_s, "trimRight"_s, "trimStart"_s, "big"_s, "blink"_s, "bold"_s, "fixed"_s, "italics"_s, "small"_s, "strike"_s, "sub"_s, "sup"_s })
        method("String"_s, name, { }, string, Pure);
    for (auto name : { "anchor"_s, "fontcolor"_s, "fontsize"_s, "link"_s })
        method("String"_s, name, { toString }, string, Pure);
    method("String"_s, "toString"_s, { }, string, Pure, inlined(InlineBuiltin::This));
    method("String"_s, "valueOf"_s, { }, string, Pure, inlined(InlineBuiltin::This));
    method("String"_s, "@@iterator"_s, { }, object, Makes);
    methodsThatSayNothing("String"_s, { "matchAll"_s });

    // RegExp. Where it is at, and the last match, only the engine looks at.
    const BuiltinResult newRegExp = fresh("RegExp"_s);
    function("RegExp"_s, { optional(toString), optional(toString) }, newRegExp, Makes | Throws);
    function("RegExp"_s, { rest(asIs) }, anything, Unknown);
    constructor("RegExp"_s, { optional(toString), optional(toString) }, newRegExp, Makes | Throws);
    constructor("RegExp"_s, { rest(asIs) }, newRegExp, Unknown);
    staticFunction("RegExp"_s, "escape"_s, { asIs }, string, Throws);
    method("RegExp"_s, "compile"_s, { rest(asIs) }, thisValue, Unknown);
    method("RegExp"_s, "exec"_s, { toString }, newArray | null, World | Makes | Throws);
    method("RegExp"_s, "test"_s, { toString }, boolean, World | Throws);
    method("RegExp"_s, "toString"_s, { }, string, ReadsWorld);
    for (auto name : { "dotAll"_s, "global"_s, "hasIndices"_s, "ignoreCase"_s, "multiline"_s, "sticky"_s, "unicode"_s, "unicodeSets"_s })
        getter("RegExp"_s, name, boolean, ReadsWorld);
    getter("RegExp"_s, "flags"_s, string, ReadsWorld);
    getter("RegExp"_s, "source"_s, string, ReadsWorld);
    methodsThatSayNothing("RegExp"_s, { "@@match"_s, "@@matchAll"_s, "@@replace"_s, "@@search"_s, "@@split"_s });

    // JSON.
    staticFunction("JSON"_s, "parse"_s, { toString }, anything, Makes | Throws);
    staticFunction("JSON"_s, "parse"_s, { rest(asIs) }, anything, Unknown);
    staticFunction("JSON"_s, "stringify"_s, { asIs }, stringOrUndefined, ReadsArguments | Throws);
    staticFunction("JSON"_s, "stringify"_s, { rest(asIs) }, stringOrUndefined, Unknown);
    staticFunction("JSON"_s, "rawJSON"_s, { toString }, object, Makes | Throws);
    staticFunction("JSON"_s, "isRawJSON"_s, { asIs }, boolean, Pure);
}

void BuiltinsFiller::fillIndexedCollections()
{
    // Array. What makes an array by way of Symbol.species may come back with anything.
    for (auto calledOrConstructed : { &BuiltinsFiller::constructor, &BuiltinsFiller::function }) {
        (this->*calledOrConstructed)("Array"_s, { }, newArray, Makes, inlined(InlineBuiltin::ArrayOf));
        (this->*calledOrConstructed)("Array"_s, { isNumber }, newArray, Makes | Throws, inlined(InlineBuiltin::ArrayWithLength));
        (this->*calledOrConstructed)("Array"_s, { asIs }, newArray, Makes | Throws, engine);
        (this->*calledOrConstructed)("Array"_s, { asIs, asIs, rest(asIs) }, newArray, Makes, inlined(InlineBuiltin::ArrayOf));
    }
    staticFunction("Array"_s, "from"_s, { asIs }, newArray, Makes | ReadsArguments | Throws);
    staticFunction("Array"_s, "from"_s, { rest(asIs) }, newArray, Unknown);
    staticFunction("Array"_s, "fromAsync"_s, { rest(asIs) }, newPromise, Unknown);
    staticFunction("Array"_s, "isArray"_s, { asIs }, boolean, Throws, inlined(InlineBuiltin::ArrayIsArray));
    staticFunction("Array"_s, "of"_s, { rest(asIs) }, newArray, Makes, inlined(InlineBuiltin::ArrayOf));
    // What changes nothing runs no code of the program either. On an array that has become one that would have it do so, it throws: see
    // operationAOTTypedCallReaderOfArray.
    getter("Array"_s, "length"_s, arrayLength, Reads, inlined(InlineBuiltin::ArrayLength));
    setter("Array"_s, "length"_s, isNumber, Writes | Throws, inlined(InlineBuiltin::ArraySetLength));
    method("Array"_s, "at"_s, { toNumber }, anything, Reads | Throws);
    method("Array"_s, "copyWithin"_s, { toNumber, toNumber, optional(toNumber) }, thisValue, Writes);
    for (auto name : { "entries"_s, "keys"_s, "values"_s, "@@iterator"_s })
        method("Array"_s, name, { }, object, Makes | Throws);
    method("Array"_s, "every"_s, { rest(asIs) }, boolean, Unknown);
    method("Array"_s, "some"_s, { rest(asIs) }, boolean, Unknown);
    method("Array"_s, "fill"_s, { asIs, optional(toNumber), optional(toNumber) }, thisValue, Writes);
    method("Array"_s, "findIndex"_s, { rest(asIs) }, placeInArrayOrMinusOne, Unknown);
    method("Array"_s, "findLastIndex"_s, { rest(asIs) }, placeInArrayOrMinusOne, Unknown);
    method("Array"_s, "forEach"_s, { rest(asIs) }, undefined, Unknown);
    method("Array"_s, "includes"_s, { asIs, optional(toNumber) }, boolean, Reads | Throws);
    method("Array"_s, "indexOf"_s, { asIs, optional(toNumber) }, placeInArrayOrMinusOne, Reads | Throws);
    method("Array"_s, "lastIndexOf"_s, { asIs, optional(toNumber) }, placeInArrayOrMinusOne, Reads | Throws);
    method("Array"_s, "join"_s, { rest(asIs) }, string, Unknown);
    method("Array"_s, "toLocaleString"_s, { rest(asIs) }, string, Unknown);
    method("Array"_s, "pop"_s, { }, anything, Writes);
    method("Array"_s, "shift"_s, { }, anything, Writes);
    method("Array"_s, "push"_s, { rest(asIs) }, arrayLength, Writes | Throws, inlined(InlineBuiltin::ArrayPush));
    method("Array"_s, "unshift"_s, { rest(asIs) }, arrayLength, Writes | Throws);
    method("Array"_s, "reverse"_s, { }, thisValue, Writes);
    method("Array"_s, "sort"_s, { rest(asIs) }, thisValue, Unknown);
    method("Array"_s, "toReversed"_s, { }, newArray, Copies | Throws);
    method("Array"_s, "toSorted"_s, { rest(asIs) }, newArray, Unknown);
    method("Array"_s, "toSpliced"_s, { optional(toNumber), optional(toNumber), rest(asIs) }, newArray, Copies | Throws);
    method("Array"_s, "with"_s, { toNumber, asIs }, newArray, Copies | Throws);
    methodsThatSayNothing("Array"_s, { "concat"_s, "filter"_s, "find"_s, "findLast"_s, "flat"_s, "flatMap"_s, "map"_s, "reduce"_s, "reduceRight"_s, "slice"_s, "splice"_s, "toString"_s });

    // Typed arrays.
#define AOT_TYPED_ADD_BYTES_PER_ELEMENT(name) \
    constant(#name "Array"_s, "BYTES_PER_ELEMENT"_s, elementSize(Type ## name)); \
    getter(#name "Array"_s, "BYTES_PER_ELEMENT"_s, number, Pure, is(elementSize(Type ## name)));
    FOR_EACH_TYPED_ARRAY_TYPE_EXCLUDING_DATA_VIEW(AOT_TYPED_ADD_BYTES_PER_ELEMENT)
#undef AOT_TYPED_ADD_BYTES_PER_ELEMENT
    constructor("TypedArray"_s, { }, sameClass, Makes);
    constructor("TypedArray"_s, { isNumber }, sameClass, Makes | Throws, inlined(InlineBuiltin::TypedArrayWithLength));
    constructor("TypedArray"_s, { rest(asIs) }, sameClass, Unknown);
    staticFunction("TypedArray"_s, "from"_s, { rest(asIs) }, sameClass, Unknown);
    staticFunction("TypedArray"_s, "of"_s, { rest(toNumber) }, sameClass, Makes);
    // Asked for its buffer, it may move its elements there.
    getter("TypedArray"_s, "buffer"_s, object, Unknown);
    getter("TypedArray"_s, "byteLength"_s, bufferLength, Reads);
    getter("TypedArray"_s, "byteOffset"_s, bufferLength, Reads);
    getter("TypedArray"_s, "length"_s, bufferLength, Reads, inlined(InlineBuiltin::TypedArrayLength));
    // Every method turns away a typed array whose buffer has been detached.
    method("TypedArray"_s, "at"_s, { toNumber }, anything, Reads | Throws);
    method("TypedArray"_s, "copyWithin"_s, { toNumber, toNumber, optional(toNumber) }, thisValue, Writes | Throws);
    for (auto name : { "entries"_s, "keys"_s, "values"_s, "@@iterator"_s })
        method("TypedArray"_s, name, { }, object, Makes | Throws);
    method("TypedArray"_s, "every"_s, { rest(asIs) }, boolean, Unknown);
    method("TypedArray"_s, "some"_s, { rest(asIs) }, boolean, Unknown);
    method("TypedArray"_s, "fill"_s, { toNumber, optional(toNumber), optional(toNumber) }, thisValue, Writes | Throws);
    method("TypedArray"_s, "findIndex"_s, { rest(asIs) }, placeInBufferOrMinusOne, Unknown);
    method("TypedArray"_s, "findLastIndex"_s, { rest(asIs) }, placeInBufferOrMinusOne, Unknown);
    method("TypedArray"_s, "forEach"_s, { rest(asIs) }, undefined, Unknown);
    method("TypedArray"_s, "includes"_s, { asIs, optional(toNumber) }, boolean, Reads | Throws);
    method("TypedArray"_s, "indexOf"_s, { asIs, optional(toNumber) }, placeInBufferOrMinusOne, Reads | Throws);
    method("TypedArray"_s, "lastIndexOf"_s, { asIs, optional(toNumber) }, placeInBufferOrMinusOne, Reads | Throws);
    method("TypedArray"_s, "join"_s, { optional(toString) }, string, Reads | Throws);
    method("TypedArray"_s, "toLocaleString"_s, { rest(asIs) }, string, Unknown);
    method("TypedArray"_s, "reverse"_s, { }, thisValue, Writes | Throws);
    method("TypedArray"_s, "set"_s, { isTypedArray, optional(toNumber) }, undefined, Writes | Throws);
    method("TypedArray"_s, "set"_s, { rest(asIs) }, undefined, Unknown);
    method("TypedArray"_s, "sort"_s, { }, thisValue, Writes | Throws);
    method("TypedArray"_s, "sort"_s, { rest(asIs) }, thisValue, Unknown);
    method("TypedArray"_s, "toReversed"_s, { }, sameClass, Copies | Throws);
    method("TypedArray"_s, "toSorted"_s, { }, sameClass, Copies | Throws);
    method("TypedArray"_s, "toSorted"_s, { rest(asIs) }, sameClass, Unknown);
    method("TypedArray"_s, "with"_s, { toNumber, toNumber }, sameClass, Copies | Throws);
    methodsThatSayNothing("TypedArray"_s, { "filter"_s, "find"_s, "findLast"_s, "map"_s, "reduce"_s, "reduceRight"_s, "slice"_s, "subarray"_s, "toString"_s });
    staticFunction("Uint8Array"_s, "fromBase64"_s, { rest(asIs) }, fresh("Uint8Array"_s), Unknown);
    staticFunction("Uint8Array"_s, "fromHex"_s, { toString }, fresh("Uint8Array"_s), Makes | Throws);
    method("Uint8Array"_s, "toBase64"_s, { }, string, Reads | Throws);
    method("Uint8Array"_s, "toBase64"_s, { rest(asIs) }, string, Unknown);
    method("Uint8Array"_s, "toHex"_s, { }, string, Reads | Throws);
    method("Uint8Array"_s, "setFromBase64"_s, { rest(asIs) }, object, Unknown);
    method("Uint8Array"_s, "setFromHex"_s, { toString }, object, Writes | Makes | Throws);

    // Buffers. Typed arrays look at their bytes, and go by their lengths.
    const BuiltinResult newArrayBuffer = fresh("ArrayBuffer"_s);
    constructor("ArrayBuffer"_s, { toNumber }, newArrayBuffer, Makes | Throws);
    constructor("ArrayBuffer"_s, { rest(asIs) }, newArrayBuffer, Unknown);
    staticFunction("ArrayBuffer"_s, "isView"_s, { asIs }, boolean, Pure);
    getter("ArrayBuffer"_s, "byteLength"_s, bufferLength, Reads);
    getter("ArrayBuffer"_s, "detached"_s, boolean, Reads);
    getter("ArrayBuffer"_s, "maxByteLength"_s, bufferLength, Reads);
    getter("ArrayBuffer"_s, "resizable"_s, boolean, Pure);
    method("ArrayBuffer"_s, "resize"_s, { toNumber }, undefined, Writes | Throws);
    method("ArrayBuffer"_s, "slice"_s, { rest(asIs) }, newArrayBuffer, Unknown);
    method("ArrayBuffer"_s, "transfer"_s, { optional(toNumber) }, newArrayBuffer, Writes | Makes | Throws);
    method("ArrayBuffer"_s, "transferToFixedLength"_s, { optional(toNumber) }, newArrayBuffer, Writes | Makes | Throws);
    const BuiltinResult newSharedArrayBuffer = fresh("SharedArrayBuffer"_s);
    constructor("SharedArrayBuffer"_s, { rest(asIs) }, newSharedArrayBuffer, Unknown);
    getter("SharedArrayBuffer"_s, "byteLength"_s, bufferLength, Reads | ReadsWorld);
    getter("SharedArrayBuffer"_s, "growable"_s, boolean, Pure);
    getter("SharedArrayBuffer"_s, "maxByteLength"_s, bufferLength, Pure);
    method("SharedArrayBuffer"_s, "grow"_s, { toNumber }, undefined, Writes | World | Throws);
    method("SharedArrayBuffer"_s, "slice"_s, { rest(asIs) }, newSharedArrayBuffer, Unknown);

    // DataView.
    constructor("DataView"_s, { rest(asIs) }, fresh("DataView"_s), Unknown);
    getter("DataView"_s, "byteLength"_s, bufferLength, Reads | Throws);
    method("DataView"_s, "getBigInt64"_s, { toNumber, optional(asIs) }, bigInt, Reads | Makes | Throws);
    method("DataView"_s, "getBigUint64"_s, { toNumber, optional(asIs) }, bigInt, Reads | Makes | Throws);
    for (auto name : { "getFloat16"_s, "getFloat32"_s, "getFloat64"_s })
        method("DataView"_s, name, { toNumber, optional(asIs) }, number, Reads | Throws);
    method("DataView"_s, "getInt8"_s, { toNumber }, integer(-128, 127), Reads | Throws);
    method("DataView"_s, "getInt16"_s, { toNumber, optional(asIs) }, integer(-32768, 32767), Reads | Throws);
    method("DataView"_s, "getInt32"_s, { toNumber, optional(asIs) }, int32, Reads | Throws);
    method("DataView"_s, "getUint8"_s, { toNumber }, integer(0, 255), Reads | Throws);
    method("DataView"_s, "getUint16"_s, { toNumber, optional(asIs) }, integer(0, 65535), Reads | Throws);
    method("DataView"_s, "getUint32"_s, { toNumber, optional(asIs) }, uint32, Reads | Throws);
    method("DataView"_s, "setBigInt64"_s, { toNumber, asIs, optional(asIs) }, undefined, Writes | ReadsArguments | Throws);
    method("DataView"_s, "setBigUint64"_s, { toNumber, asIs, optional(asIs) }, undefined, Writes | ReadsArguments | Throws);
    for (auto name : { "setFloat16"_s, "setFloat32"_s, "setFloat64"_s, "setInt16"_s, "setInt32"_s, "setUint16"_s, "setUint32"_s })
        method("DataView"_s, name, { toNumber, toNumber, optional(asIs) }, undefined, Writes | Throws);
    method("DataView"_s, "setInt8"_s, { toNumber, toNumber }, undefined, Writes | Throws);
    method("DataView"_s, "setUint8"_s, { toNumber, toNumber }, undefined, Writes | Throws);

    // Atomics.
    staticFunctionsThatSayNothing("Atomics"_s, { "add"_s, "and"_s, "compareExchange"_s, "exchange"_s, "load"_s, "or"_s, "store"_s, "sub"_s, "waitAsync"_s, "xor"_s });
    staticFunction("Atomics"_s, "isLockFree"_s, { toNumber }, boolean, Pure);
    staticFunction("Atomics"_s, "notify"_s, { rest(asIs) }, number, Unknown);
    staticFunction("Atomics"_s, "pause"_s, { optional(asIs) }, undefined, World | Throws);
    staticFunction("Atomics"_s, "wait"_s, { rest(asIs) }, string, Unknown);
}

// What is in them only the engine looks into.
void BuiltinsFiller::fillKeyedCollections()
{
    // Map.
    const BuiltinResult newMap = fresh("Map"_s);
    constructor("Map"_s, { }, newMap, Makes, inlined(InlineBuiltin::MapNew));
    constructor("Map"_s, { asIs }, newMap, Makes | ReadsArguments | Throws);
    staticFunction("Map"_s, "groupBy"_s, { rest(asIs) }, newMap, Unknown);
    getter("Map"_s, "size"_s, sizeOfMapOrSet, ReadsWorld, inlined(InlineBuiltin::MapSize));
    method("Map"_s, "clear"_s, { }, undefined, World);
    method("Map"_s, "delete"_s, { asIs }, boolean, World);
    for (auto name : { "entries"_s, "keys"_s, "values"_s, "@@iterator"_s })
        method("Map"_s, name, { }, object, Makes);
    method("Map"_s, "forEach"_s, { rest(asIs) }, undefined, Unknown);
    method("Map"_s, "get"_s, { asIs }, anything, ReadsWorld, inlined(InlineBuiltin::MapGet));
    method("Map"_s, "getOrInsert"_s, { asIs, asIs }, anything, World);
    method("Map"_s, "has"_s, { asIs }, boolean, ReadsWorld, inlined(InlineBuiltin::MapHas));
    method("Map"_s, "set"_s, { asIs, asIs }, thisValue, World, inlined(InlineBuiltin::MapSet));
    methodsThatSayNothing("Map"_s, { "getOrInsertComputed"_s });

    // Set.
    const BuiltinResult newSet = fresh("Set"_s);
    constructor("Set"_s, { }, newSet, Makes);
    constructor("Set"_s, { asIs }, newSet, Makes | ReadsArguments | Throws);
    getter("Set"_s, "size"_s, sizeOfMapOrSet, ReadsWorld, inlined(InlineBuiltin::SetSize));
    method("Set"_s, "add"_s, { asIs }, thisValue, World, inlined(InlineBuiltin::SetAdd));
    method("Set"_s, "clear"_s, { }, undefined, World);
    method("Set"_s, "delete"_s, { asIs }, boolean, World);
    for (auto name : { "entries"_s, "keys"_s, "values"_s, "@@iterator"_s })
        method("Set"_s, name, { }, object, Makes);
    method("Set"_s, "forEach"_s, { rest(asIs) }, undefined, Unknown);
    method("Set"_s, "has"_s, { asIs }, boolean, ReadsWorld, inlined(InlineBuiltin::SetHas));
    // The other one is asked for its `size`, `has` and `keys`.
    for (auto name : { "difference"_s, "intersection"_s, "symmetricDifference"_s, "union"_s })
        method("Set"_s, name, { rest(asIs) }, newSet, Unknown);
    for (auto name : { "isDisjointFrom"_s, "isSubsetOf"_s, "isSupersetOf"_s })
        method("Set"_s, name, { rest(asIs) }, boolean, Unknown);

    // What holds on weakly.
    constructor("WeakMap"_s, { }, fresh("WeakMap"_s), Makes);
    constructor("WeakMap"_s, { rest(asIs) }, fresh("WeakMap"_s), Unknown);
    method("WeakMap"_s, "delete"_s, { asIs }, boolean, World);
    method("WeakMap"_s, "get"_s, { asIs }, anything, ReadsWorld);
    method("WeakMap"_s, "getOrInsert"_s, { asIs, asIs }, anything, World | Throws);
    method("WeakMap"_s, "has"_s, { asIs }, boolean, ReadsWorld);
    method("WeakMap"_s, "set"_s, { asIs, asIs }, thisValue, World | Throws);
    methodsThatSayNothing("WeakMap"_s, { "getOrInsertComputed"_s });
    constructor("WeakSet"_s, { }, fresh("WeakSet"_s), Makes);
    constructor("WeakSet"_s, { rest(asIs) }, fresh("WeakSet"_s), Unknown);
    method("WeakSet"_s, "add"_s, { asIs }, thisValue, World | Throws);
    method("WeakSet"_s, "delete"_s, { asIs }, boolean, World);
    method("WeakSet"_s, "has"_s, { asIs }, boolean, ReadsWorld);
    constructor("WeakRef"_s, { asIs }, fresh("WeakRef"_s), Makes | Throws);
    method("WeakRef"_s, "deref"_s, { }, anything, ReadsWorld);
    // The function is called some other time.
    constructor("FinalizationRegistry"_s, { asIs }, fresh("FinalizationRegistry"_s), Makes | Throws);
    method("FinalizationRegistry"_s, "register"_s, { asIs, asIs, optional(asIs) }, undefined, World | Throws);
    method("FinalizationRegistry"_s, "unregister"_s, { asIs }, boolean, World | Throws);
}

void BuiltinsFiller::fillControlAbstractions()
{
    // Promise. What is made on the class itself is a promise. What is made from a promise goes through Symbol.species.
    constructor("Promise"_s, { rest(asIs) }, newPromise, Unknown);
    for (auto name : { "all"_s, "any"_s, "race"_s })
        staticFunction("Promise"_s, name, { asIs }, newPromise, Makes | ReadsArguments | World);
    staticFunction("Promise"_s, "resolve"_s, { optional(asIs) }, newPromise, Makes | ReadsArguments | World);
    for (auto name : { "allSettled"_s, "reject"_s, "try"_s })
        staticFunction("Promise"_s, name, { rest(asIs) }, newPromise, Unknown);
    staticFunction("Promise"_s, "withResolvers"_s, { }, object, Makes);
    methodsThatSayNothing("Promise"_s, { "then"_s, "catch"_s, "finally"_s });

    // Reflect and Proxy.
    staticFunctionsThatSayNothing("Reflect"_s, { "apply"_s, "get"_s });
    staticFunction("Reflect"_s, "construct"_s, { rest(asIs) }, object, Unknown);
    for (auto name : { "defineProperty"_s, "deleteProperty"_s, "preventExtensions"_s, "set"_s, "setPrototypeOf"_s })
        staticFunction("Reflect"_s, name, { rest(asIs) }, boolean, Unknown);
    staticFunction("Reflect"_s, "getOwnPropertyDescriptor"_s, { rest(asIs) }, object | undefined, Unknown);
    staticFunction("Reflect"_s, "getPrototypeOf"_s, { asIs }, object | null, ReadsArguments | Throws);
    staticFunction("Reflect"_s, "has"_s, { asIs, toPropertyKey }, boolean, ReadsArguments | Throws);
    staticFunction("Reflect"_s, "isExtensible"_s, { asIs }, boolean, ReadsArguments | Throws);
    staticFunction("Reflect"_s, "ownKeys"_s, { asIs }, newArray, ReadsArguments | Makes | Throws);
    constructor("Proxy"_s, { rest(asIs) }, object, Unknown);
    staticFunction("Proxy"_s, "revocable"_s, { rest(asIs) }, object, Unknown);

    // Iterators. Each step calls `next`.
    staticFunctionsThatSayNothing("Iterator"_s, { "concat"_s, "from"_s, "zip"_s, "zipKeyed"_s });
    for (auto name : { "drop"_s, "filter"_s, "flatMap"_s, "map"_s, "take"_s })
        method("Iterator"_s, name, { rest(asIs) }, object, Unknown);
    method("Iterator"_s, "every"_s, { rest(asIs) }, boolean, Unknown);
    method("Iterator"_s, "some"_s, { rest(asIs) }, boolean, Unknown);
    method("Iterator"_s, "forEach"_s, { rest(asIs) }, undefined, Unknown);
    method("Iterator"_s, "toArray"_s, { rest(asIs) }, newArray, Unknown);
    method("Iterator"_s, "@@iterator"_s, { }, thisValue, Pure, inlined(InlineBuiltin::This));
    methodsThatSayNothing("Iterator"_s, { "chunks"_s, "find"_s, "includes"_s, "join"_s, "reduce"_s, "windows"_s, "@@dispose"_s });

    // What is disposed of.
    constructor("DisposableStack"_s, { }, object, Makes);
    constructor("AsyncDisposableStack"_s, { }, object, Makes);
}

void BuiltinsFiller::fill()
{
    m_table.m_typedArray = intern("TypedArray"_s);
    m_table.m_prototype = intern("prototype"_s);
    m_table.m_call = intern("call"_s);
    engineClass("Array"_s, ArrayType);
    engineClass("Map"_s, JSMapType);
    engineClass("Set"_s, JSSetType);
    engineClass("Promise"_s, JSPromiseType);
    engineClass("Date"_s, JSDateType);
    engineClass("RegExp"_s, RegExpObjectType);
    engineClass("Error"_s, ErrorInstanceType);
    engineClass("DataView"_s, DataViewType);
    engineClass("WeakMap"_s, JSWeakMapType);
    engineClass("WeakSet"_s, JSWeakSetType);
    // The two are one class to the engine, which tells them apart where a method is called: that of the one turns away the other.
    engineClass("ArrayBuffer"_s, ArrayBufferType);
    engineClass("SharedArrayBuffer"_s, ArrayBufferType);
#define AOT_TYPED_ADD_TYPED_ARRAY_CLASS(name) engineClass(#name "Array"_s, "TypedArray"_s, Type ## name);
    FOR_EACH_TYPED_ARRAY_TYPE_EXCLUDING_DATA_VIEW(AOT_TYPED_ADD_TYPED_ARRAY_CLASS)
#undef AOT_TYPED_ADD_TYPED_ARRAY_CLASS
    fillOperations();
    fillHostObjects();
    fillFundamentalObjects();
    fillNumbersAndDates();
    fillText();
    fillIndexedCollections();
    fillKeyedCollections();
    fillControlAbstractions();

    // The typed arrays have a class in common that the global object has no name for.
    m_table.m_isGlobal[m_table.m_typedArray.index()] = false;
    // An object of a class that extends one of these has its type, and may have other methods. One that extends Array has a type of its own.
    for (auto& candidate : m_table.m_classes) {
        if (candidate.type != CellType && candidate.type != ArrayType)
            m_table.m_mayBeExtended[candidate.name.index()] = true;
    }
}

Builtins::Builtins()
{
    BuiltinsFiller(*this).fill();
#define AOT_TYPED_FIND_CLASS(lower, upper) m_ ## lower ## Class = classNamed(nameOf(#upper ""_s.span8()))->index;
    FOR_EACH_AOT_TYPED_WELL_KNOWN_CLASS(AOT_TYPED_FIND_CLASS)
#undef AOT_TYPED_FIND_CLASS
}

const Builtins& Builtins::singleton()
{
    static LazyNeverDestroyed<Builtins> table;
    static std::once_flag onceFlag;
    std::call_once(onceFlag, [] {
        table.construct();
    });
    return table;
}

// `@@name` is how the table spells a symbol. A string of a program that reads like that is no such name.
static bool spellsSymbol(std::span<const uint8_t> bytes)
{
    return bytes.size() > 2 && bytes[0] == '@' && bytes[1] == '@';
}

BuiltinName Builtins::nameOf(std::span<const uint8_t> bytes) const
{
    if (bytes.empty() || spellsSymbol(bytes))
        return { };
    auto iterator = m_names.find(String(byteCast<Latin1Character>(bytes)));
    if (iterator == m_names.end())
        return { };
    return BuiltinName(iterator->value);
}

BuiltinID Builtins::find(BuiltinKind kind, BuiltinName holder, BuiltinName name) const
{
    if (!name)
        return { };
    auto iterator = m_first.find(keyFor(kind, holder, name));
    if (iterator == m_first.end())
        return { };
    return BuiltinID(iterator->value);
}

BuiltinID Builtins::next(BuiltinID id) const
{
    unsigned index = id.index() + 1;
    if (index >= m_entries.size())
        return { };
    auto& previous = m_entries[id.index()];
    auto& entry = m_entries[index];
    if (entry.kind != previous.kind || entry.holderName != previous.holderName || entry.nameName != previous.nameName)
        return { };
    return BuiltinID(index);
}

BuiltinID Builtins::findForInstance(BuiltinKind kind, const EngineClass& engineClass, BuiltinName name) const
{
    if (BuiltinID found = find(kind, engineClass.name, name))
        return found;
    if (engineClass.listedAs != engineClass.name) {
        if (BuiltinID found = find(kind, engineClass.listedAs, name))
            return found;
    }
    return find(kind, objectClass().name, name);
}

BuiltinID Builtins::findStatic(BuiltinKind kind, BuiltinName global, BuiltinName name) const
{
    if (!global)
        return { };
    if (BuiltinID found = find(kind, global, name))
        return found;
    const EngineClass* engineClass = classNamed(global);
    if (!engineClass || engineClass->listedAs == engineClass->name)
        return { };
    return find(kind, engineClass->listedAs, name);
}

BuiltinID Builtins::findConstructor(BuiltinName global) const
{
    const EngineClass* engineClass = classNamed(global);
    return find(BuiltinKind::Constructor, { }, engineClass ? engineClass->listedAs : global);
}

const EngineClass* Builtins::classNamed(BuiltinName name) const
{
    if (!name || m_classOfName[name.index()] == UINT16_MAX)
        return nullptr;
    return &m_classes[m_classOfName[name.index()]];
}

GlobalInterface Builtins::interfaceNamed(BuiltinName name) const
{
    if (!name)
        return GlobalInterface::Unknown;
    if (name == functionClass().name)
        return GlobalInterface::Function;
    if (name == stringClass().name)
        return GlobalInterface::String;
    if (name == numberClass().name)
        return GlobalInterface::Number;
    if (name == booleanClass().name)
        return GlobalInterface::Boolean;
    if (name == symbolClass().name)
        return GlobalInterface::Symbol;
    if (name == bigIntClass().name)
        return GlobalInterface::BigInt;
    return classNamed(name) ? GlobalInterface::Class : GlobalInterface::Unknown;
}

// The properties that are looked at here cannot be replaced, so it does not matter when this is asked.
JSCell* Builtins::findCell(JSGlobalObject* globalObject, unsigned index) const
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    auto objectAt = [&](JSObject* holder, const Identifier& name) {
        JSValue value = holder->get(globalObject, name);
        scope.assertNoException();
        RELEASE_ASSERT(value.isObject());
        return asObject(value);
    };
    auto global = [&](ASCIILiteral name) {
        return objectAt(globalObject, Identifier::fromString(vm, name));
    };
    if (index >= m_entries.size())
        return global(m_texts[index - m_entries.size()]);

    auto& builtin = m_entries[index];
    JSObject* holder = builtin.holderName == m_typedArray ? asObject(global("Int8Array"_s)->getPrototypeDirect()) : global(builtin.holder);
    if (builtin.takesReceiver())
        holder = objectAt(holder, vm.propertyNames->prototype);
    Identifier key = Identifier::fromString(vm, builtin.name);
    if (spellsSymbol(builtin.name.span8())) {
        JSValue symbol = global("Symbol"_s)->get(globalObject, Identifier::fromString(vm, builtin.name.span8().subspan(2)));
        scope.assertNoException();
        key = Identifier::fromUid(asSymbol(symbol)->privateName());
    }
    PropertySlot slot(holder, PropertySlot::InternalMethodType::GetOwnProperty);
    bool isFound = holder->methodTable()->getOwnPropertySlot(holder, globalObject, key, slot);
    scope.assertNoException();
    RELEASE_ASSERT(isFound && (slot.isAccessor() || builtin.kind == BuiltinKind::Method || !builtin.takesReceiver()));
    JSValue value;
    if (builtin.kind == BuiltinKind::Getter)
        value = slot.getterSetter()->getter();
    else if (builtin.kind == BuiltinKind::Setter)
        value = slot.getterSetter()->setter();
    else
        value = slot.getValue(globalObject, key);
    scope.assertNoException();
    RELEASE_ASSERT(value.isCell());
    return value.asCell();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
