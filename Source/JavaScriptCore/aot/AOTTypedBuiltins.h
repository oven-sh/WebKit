/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "JSType.h"
#include "TypedArrayType.h"
#include <wtf/HashMap.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/Vector.h>
#include <wtf/text/ASCIILiteral.h>
#include <wtf/text/StringHash.h>
#include <wtf/text/WTFString.h>

namespace JSC {

class JSCell;
class JSGlobalObject;

namespace AOT { namespace Typed {

// Every built-in of the language and every abstract operation the front end names. No other file knows a built-in by its name.
//
// An entry says three things: which arguments it holds for, what the engine vouches for about what it yields, and what it does besides.
// It holds for a receiver of the class it is listed under, which is as the engine makes them (its elements are data, it has the
// prototype it was made with), and for arguments whose conversion runs no code of the program.
//
// What comes down to an entry is carried out by code in place or by the function of the engine itself, see Builtins::findCell(). The name
// is not looked up in the receiver: whatever else might be found there would be bound by nothing the entry says.

// A name the table knows, as a number. The strings of a program are looked up once each.
class BuiltinName {
public:
    constexpr BuiltinName() = default;
    constexpr explicit BuiltinName(unsigned index)
        : m_index(static_cast<uint16_t>(index))
    {
    }

    constexpr explicit operator bool() const { return m_index != UINT16_MAX; }
    constexpr unsigned index() const { return m_index; }
    friend constexpr bool operator==(BuiltinName, BuiltinName) = default;

private:
    uint16_t m_index { UINT16_MAX };
};

enum class BuiltinKind : uint8_t {
    Function,
    Constructor,
    StaticFunction,
    Constant,
    Method,
    Getter,
    Setter,
    Operation,
};

enum class ArgumentConversion : uint8_t {
    None,
    RequiresNumber,
    // What is looked at is its bytes and how many there are, which asks nothing of the object.
    RequiresTypedArray,
    // Also what goes on from there: ToIntegerOrInfinity, ToInt32, ToUint32.
    ToNumber,
    ToString,
    ToPropertyKey,
};

enum class ParameterMode : uint8_t {
    Required,
    Optional,
    Rest,
};

struct BuiltinParameter {
    ArgumentConversion conversion { ArgumentConversion::None };
    ParameterMode mode { ParameterMode::Required };
};

// What the engine vouches for: what the entry yields is one of these, whatever it is passed. Where that is within the type of the
// value, nothing is tested.
struct BuiltinResult {
    static constexpr uint16_t Undefined = 1 << 0;
    static constexpr uint16_t Null = 1 << 1;
    static constexpr uint16_t Boolean = 1 << 2;
    // From `least` to `most`.
    static constexpr uint16_t Integer = 1 << 3;
    static constexpr uint16_t Number = 1 << 4;
    static constexpr uint16_t String = 1 << 5;
    static constexpr uint16_t Symbol = 1 << 6;
    static constexpr uint16_t BigInt = 1 << 7;
    // An object of `engineClass`.
    static constexpr uint16_t Instance = 1 << 8;
    // An object of the class of the receiver, or of the class the function or constructor was found on: Value::engineClass().
    static constexpr uint16_t InstanceOfSameClass = 1 << 9;
    static constexpr uint16_t This = 1 << 10;

    uint16_t kinds { 0 };
    // Among the classes of the table.
    uint16_t engineClass { 0 };
    // A bit for each argument it may be. One that is not passed is `undefined`.
    uint8_t arguments { 0 };
    int64_t least { 0 };
    int64_t most { 0 };

    bool isAnything() const { return !kinds && !arguments; }
};

// Of a call that the entry holds for.
namespace Effect {
static constexpr uint16_t Pure = 0;
static constexpr uint16_t Throws = 1 << 0;
static constexpr uint16_t Allocates = 1 << 1;
static constexpr uint16_t ReadsThis = 1 << 2;
static constexpr uint16_t WritesThis = 1 << 3;
// It looks into the objects among its arguments, which runs their accessors and traps.
static constexpr uint16_t ReadsArguments = 1 << 4;
static constexpr uint16_t WritesArguments = 1 << 5;
// What only the engine looks into: the clock, the random numbers, the registry of symbols, the queue of jobs, the console, the last match.
static constexpr uint16_t ReadsWorld = 1 << 6;
static constexpr uint16_t WritesWorld = 1 << 7;
// It calls a function it is passed, converts what a container holds, or asks Symbol.species what to make.
static constexpr uint16_t RunsUnknownCode = 1 << 8;
static constexpr uint16_t Unknown = RunsUnknownCode | Throws | Allocates | ReadsThis | WritesThis | ReadsArguments | WritesArguments | ReadsWorld | WritesWorld;
}

#define FOR_EACH_AOT_TYPED_INLINE_BUILTIN(v) \
    v(MathAbs) \
    v(MathFloor) \
    v(MathCeil) \
    v(MathTrunc) \
    v(MathSqrt) \
    v(MathPow) \
    v(MathMin) \
    v(MathMax) \
    v(MathImul) \
    v(MathClz32) \
    v(IsNaN) \
    v(IsFinite) \
    v(NumberIsNaN) \
    v(NumberIsFinite) \
    v(NumberIsInteger) \
    v(NumberIsSafeInteger) \
    v(ToNumber) \
    v(StringLength) \
    v(StringAt) \
    v(StringCharAt) \
    v(StringCharCodeAt) \
    v(StringCodePointAt) \
    v(StringSlice) \
    v(StringSubstring) \
    v(StringSubstr) \
    v(StringStartsWith) \
    v(StringEndsWith) \
    v(StringIncludes) \
    v(StringIndexOf) \
    v(ArrayLength) \
    v(ArraySetLength) \
    v(ArrayPush) \
    v(ArrayWithLength) \
    v(ArrayOf) \
    v(ArrayIsArray) \
    v(TypedArrayWithLength) \
    v(TypedArrayLength) \
    v(MapNew) \
    v(MapGet) \
    v(MapSet) \
    v(MapHas) \
    v(MapSize) \
    v(SetAdd) \
    v(SetHas) \
    v(SetSize) \
    v(This)

enum class InlineBuiltin : uint8_t {
#define AOT_TYPED_DECLARE_INLINE_BUILTIN(name) name,
    FOR_EACH_AOT_TYPED_INLINE_BUILTIN(AOT_TYPED_DECLARE_INLINE_BUILTIN)
#undef AOT_TYPED_DECLARE_INLINE_BUILTIN
};

// What the language does and no built-in function of the same name: the front end asks for these by name.
#define FOR_EACH_AOT_TYPED_LANGUAGE_OPERATION(v) \
    v(CopyDataProperties) \
    v(CopyDataPropertiesExcluding) \
    v(DefineField) \
    v(DefineGetter) \
    v(DefineSetter) \
    v(ForInKeys) \
    v(AppendSpread) \
    v(AppendHole) \
    v(Apply) \
    v(ConstructWithArray) \
    v(ConstructWith) \
    v(ConstructWithSpread) \
    v(MakeClass) \
    v(NewObjectOfClass) \
    v(DefineMethod) \
    v(SuperGet) \
    v(SuperSet) \
    v(SuperConstruct) \
    v(ConstructOn) \
    v(ConstructorResult) \
    v(NewPrivateName) \
    v(PrivateGet) \
    v(PrivateSet) \
    v(PrivateHas) \
    v(PrivateFieldAdd) \
    v(PrivateMethodsAdd) \
    v(HasEnumerableProperty) \
    v(NewPromise) \
    v(ResolvePromise) \
    v(RejectPromise) \
    v(Await) \
    v(ImportExternalModule) \
    v(NewGenerator) \
    v(NewAsyncGenerator) \
    v(GetAsyncIterator) \
    v(CreateArguments) \
    v(GetIterator) \
    v(NewDisposableStack) \
    v(AddDisposableResource) \
    v(DisposeNextResource) \
    v(RecordDisposeError) \
    v(DisposeResources) \
    v(ToESM) \
    v(ParseJSON) \
    v(RequireExternalModule) \
    v(GetRequireFunction) \
    v(GetTemplateObject) \
    v(GetImportMeta) \
    v(RequireIteratorResultObject) \
    v(IteratorCloseAndThrowMissingThrowMethod)

enum class LanguageOperation : uint8_t {
#define AOT_TYPED_DECLARE_LANGUAGE_OPERATION(name) name,
    FOR_EACH_AOT_TYPED_LANGUAGE_OPERATION(AOT_TYPED_DECLARE_LANGUAGE_OPERATION)
#undef AOT_TYPED_DECLARE_LANGUAGE_OPERATION
};

struct BuiltinImplementation {
    enum Kind : uint8_t {
        Engine,
        Inline,
        Number,
        Operation,
    };

    Kind kind { Engine };
    InlineBuiltin inlined { InlineBuiltin::This };
    double number { 0 };
    LanguageOperation operation { LanguageOperation::CopyDataProperties };
};

struct Builtin {
    BuiltinKind kind;
    ASCIILiteral holder;
    ASCIILiteral name;
    BuiltinName holderName;
    BuiltinName nameName;
    Vector<BuiltinParameter, 4> parameters;
    BuiltinResult result;
    uint16_t effects;
    BuiltinImplementation implementation;

    bool isUnknown() const { return effects & Effect::RunsUnknownCode; }
    bool takesReceiver() const { return kind == BuiltinKind::Method || kind == BuiltinKind::Getter || kind == BuiltinKind::Setter; }
    unsigned numberOfRequiredParameters() const;
    bool takes(unsigned numberOfArguments) const;
};

class BuiltinID {
public:
    constexpr BuiltinID() = default;
    constexpr explicit BuiltinID(unsigned index)
        : m_index(static_cast<uint16_t>(index))
    {
    }

    constexpr explicit operator bool() const { return m_index != UINT16_MAX; }
    constexpr unsigned index() const { return m_index; }
    friend constexpr bool operator==(BuiltinID, BuiltinID) = default;
    friend constexpr auto operator<=>(BuiltinID, BuiltinID) = default;

private:
    uint16_t m_index { UINT16_MAX };
};

struct EngineClass {
    ASCIILiteral text;
    uint16_t index;
    BuiltinName name;
    // Under which its methods are listed.
    BuiltinName listedAs;
    TypedArrayType typedArrayType { NotTypedArray };
    // What every instance has for its type, and nothing else has. CellType: there is no such type.
    //
    // Array is the one class of which this is two things.
    // To be TAKEN for an array (a cast, a claim, a store from code that is not typed: whatever tests a value that may be any object), a
    // value has to be of ArrayType, which says that it is as the engine makes them: see JSObject::becomeDerivedArray().
    // What HOLDS of a value that was taken for one, as long as it lives, is less, since code that is not typed may change it:
    // - it is a cell of ArrayType or DerivedArrayType whose ClassInfo is JSArray's, so it is laid out as a JSArray;
    // - it has a butterfly with an indexing header, so the length is where it is for every array;
    // - if it is of DerivedArrayType, its elements are in ArrayStorage.
    // That it has the methods, the iteration and the (absent) inherited elements of Array.prototype holds while it is of ArrayType only.
    JSType type { CellType };
    // The host has a class that extends it and replaces methods, whose objects get wherever those of this class do.
    bool isExtendedByHost { false };
};

// What a declared type that names a global interface stands for, where that is not the instances of a class.
enum class GlobalInterface : uint8_t {
    Class,
    Function,
    String,
    Number,
    Boolean,
    Symbol,
    BigInt,
    Unknown,
};

class Builtins {
    WTF_MAKE_NONCOPYABLE(Builtins);
public:
    static const Builtins& singleton();

    const Builtin& at(BuiltinID id) const { return m_entries[id.index()]; }
    unsigned size() const { return m_entries.size(); }

    BuiltinName nameOf(std::span<const uint8_t>) const;

    // The entries of one built-in, which differ in what they take, are adjacent. A lookup yields the first. No holder: the
    // global object.
    BuiltinID find(BuiltinKind, BuiltinName holder, BuiltinName name) const;
    BuiltinID next(BuiltinID) const;
    // Whether the global object has a property of this name that the table says something about. It cannot be replaced.
    bool isGlobal(BuiltinName name) const { return name && m_isGlobal[name.index()]; }
    // Programs do replace the properties of this global object (console), or extend this class. What the table says of the properties, or
    // of what the instances find, applies only if the front end has not seen the program do that: see Program::extendedGlobals.
    bool mayBeExtended(BuiltinName global) const { return global && m_mayBeExtended[global.index()]; }
    // A property of such an object: of the kind, or nothing.
    BuiltinID findStatic(BuiltinKind, BuiltinName global, BuiltinName name) const;
    // What an instance of the class finds under the name: of its class, of what that extends, of every object.
    BuiltinID findForInstance(BuiltinKind, const EngineClass&, BuiltinName) const;
    // With which `new` on the global makes an object.
    BuiltinID findConstructor(BuiltinName global) const;
    const EngineClass* classNamed(BuiltinName) const;
    const Vector<EngineClass>& classes() const { return m_classes; }
    GlobalInterface interfaceNamed(BuiltinName) const;
    // `prototype`, under which a class has what its instances inherit from, and `call`, with which a function is called on a receiver.
    BuiltinName prototypeName() const { return m_prototype; }
    BuiltinName callName() const { return m_call; }

    // The cells of the engine that compiled code asks for, by number. They are kept in Instance::typedBuiltins once they have been asked for.
    unsigned numberOfCells() const { return m_entries.size() + m_texts.size(); }
    // The function the entry describes: a method, a static function, what reads or stores an accessor. Of a constant: the symbol.
    unsigned cellOf(BuiltinID id) const { return id.index(); }
    // What the global object has under the name: isGlobal() holds for it.
    unsigned cellOf(BuiltinName global) const { return m_entries.size() + global.index(); }
    JSCell* findCell(JSGlobalObject*, unsigned) const;

#define FOR_EACH_AOT_TYPED_WELL_KNOWN_CLASS(v) \
    v(array, Array) \
    v(promise, Promise) \
    v(string, String) \
    v(number, Number) \
    v(boolean, Boolean) \
    v(symbol, Symbol) \
    v(bigInt, BigInt) \
    v(function, Function) \
    v(object, Object)
#define AOT_TYPED_DECLARE_CLASS_GETTER(lower, upper) const EngineClass& lower ## Class() const { return m_classes[m_ ## lower ## Class]; }
    FOR_EACH_AOT_TYPED_WELL_KNOWN_CLASS(AOT_TYPED_DECLARE_CLASS_GETTER)
#undef AOT_TYPED_DECLARE_CLASS_GETTER

private:
    friend class BuiltinsFiller;
    friend class LazyNeverDestroyed<Builtins>;
    Builtins();

    Vector<Builtin> m_entries;
    UncheckedKeyHashMap<String, uint16_t> m_names;
    UncheckedKeyHashMap<uint64_t, uint16_t, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_first;
    // By name.
    Vector<ASCIILiteral> m_texts;
    Vector<bool> m_isGlobal;
    Vector<bool> m_mayBeExtended;
    Vector<uint16_t> m_classOfName;
    Vector<EngineClass> m_classes;
    BuiltinName m_typedArray;
    BuiltinName m_prototype;
    BuiltinName m_call;
#define AOT_TYPED_DECLARE_CLASS_INDEX(lower, upper) uint16_t m_ ## lower ## Class { 0 };
    FOR_EACH_AOT_TYPED_WELL_KNOWN_CLASS(AOT_TYPED_DECLARE_CLASS_INDEX)
#undef AOT_TYPED_DECLARE_CLASS_INDEX
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
