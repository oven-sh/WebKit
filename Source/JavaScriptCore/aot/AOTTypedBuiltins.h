/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTTypedBuiltinLists.h"
#include "JSCJSValue.h"
#include "JSType.h"
#include "TypedArrayType.h"
#include <expected>
#include <span>
#include <wtf/Vector.h>
#include <wtf/text/ASCIILiteral.h>
#include <wtf/text/WTFString.h>

namespace JSC {

class JSCell;
class JSGlobalObject;

namespace AOT { namespace Typed {

// The built-ins table. Its source is aot/builtins/*.def: see Scripts/generate-aot-builtins.py. No other file knows a built-in by name.
// It is constant data, which that script lays out: nothing is built when a process starts, neither one that compiles nor one that runs.
//
// A call that is resolved to an entry does exactly what ECMAScript says the call by name does. The properties of built-in prototypes and
// global objects are immutable, so what a primitive receiver or a global object finds under a name is fixed. An object receiver finds the
// entry's function as long as it has its original structure (no own property, the prototype of its class). That is tested where the
// call is made, and any other receiver takes the call by name: see hasGenericFallback(). Of a replaceable global (console) nothing is
// fixed: the object and the function are read as the language says, and what is found is compared with the function the realm started
// with: see speculateOnReceivers().

static constexpr uint32_t builtinsHash = AOT_TYPED_BUILTINS_HASH;
static constexpr unsigned numberOfBuiltins = AOT_TYPED_NUMBER_OF_BUILTINS;
static constexpr unsigned numberOfBuiltinNames = AOT_TYPED_NUMBER_OF_BUILTIN_NAMES;
static constexpr unsigned numberOfEngineClasses = AOT_TYPED_NUMBER_OF_ENGINE_CLASSES;
static constexpr unsigned numberOfOriginalStructures = AOT_TYPED_NUMBER_OF_ORIGINAL_STRUCTURES;

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
    RequiresInteger,
    RequiresTypedArray,
    // Also ToIntegerOrInfinity, ToInt32, ToUint32.
    ToNumber,
    ToString,
    ToPropertyKey,
};

enum class ParameterMode : uint8_t {
    Required,
    Optional,
    Rest,
};

namespace OperandUse {
static constexpr uint16_t WritesProperties = 1 << 0;
static constexpr uint16_t WritesContents = 1 << 1;
static constexpr uint16_t Keeps = 1 << 2;
static constexpr uint16_t Calls = 1 << 3;
static constexpr uint16_t Gets = 1 << 4;
static constexpr uint16_t MayBeResult = 1 << 5;
static constexpr uint16_t KeepsWhatItHolds = 1 << 6;
static constexpr uint16_t HandsToUserCode = 1 << 7;
static constexpr uint16_t OfAllItReaches = 1 << 8;
static constexpr uint16_t Everything = (1 << 9) - 1;
}

struct BuiltinParameter {
    ArgumentConversion conversion { ArgumentConversion::None };
    ParameterMode mode { ParameterMode::Required };
    uint16_t uses { OperandUse::Everything };
};

extern const BuiltinParameter builtinParameterPool[];

struct BuiltinParameters {
    uint16_t first;
    uint16_t count;

    size_t size() const { return count; }
    bool isEmpty() const { return !count; }
    const BuiltinParameter* begin() const { return builtinParameterPool + first; }
    const BuiltinParameter* end() const { return begin() + count; }
    const BuiltinParameter& operator[](size_t index) const { return begin()[index]; }
    const BuiltinParameter& last() const { return begin()[count - 1]; }
};

// Where this is within the type of the value, the result is not tested.
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
    // Of `engineClass`.
    static constexpr uint16_t Instance = 1 << 8;
    // Of Value::engineClass().
    static constexpr uint16_t InstanceOfSameClass = 1 << 9;
    static constexpr uint16_t This = 1 << 10;

    uint16_t kinds { 0 };
    uint16_t engineClass { 0 };
    // A bit for each argument it may be. One that is not passed is `undefined`.
    uint8_t arguments { 0 };
    int64_t least { 0 };
    int64_t most { 0 };

    bool isAnything() const { return !kinds && !arguments; }
};

namespace Effect {
static constexpr uint16_t Pure = 0;
static constexpr uint16_t MayThrow = 1 << 0;
static constexpr uint16_t Allocates = 1 << 1;
static constexpr uint16_t ReadsReceiver = 1 << 2;
static constexpr uint16_t WritesReceiver = 1 << 3;
// Runs the accessors and traps of the objects among the arguments.
static constexpr uint16_t ReadsArguments = 1 << 4;
static constexpr uint16_t WritesArguments = 1 << 5;
// What no load or store of compiled code reaches: the clock, the random number generator, the symbol registry, the microtask queue, the
// console, the last match, the contents of a Map, Set, Date or RegExp.
static constexpr uint16_t ReadsRuntimeState = 1 << 6;
static constexpr uint16_t WritesRuntimeState = 1 << 7;
static constexpr uint16_t CallsUserCode = 1 << 8;
static constexpr uint16_t ClobbersWorld = CallsUserCode | MayThrow | Allocates | ReadsReceiver | WritesReceiver | ReadsArguments | WritesArguments | ReadsRuntimeState | WritesRuntimeState;
}

// A file aot/builtins/<family>.def, with its AOTTypedBuiltins<Family>.cpp.
enum class BuiltinFamily : uint8_t {
#define AOT_TYPED_DECLARE_BUILTIN_FAMILY(name) name,
    FOR_EACH_AOT_TYPED_BUILTIN_FAMILY(AOT_TYPED_DECLARE_BUILTIN_FAMILY)
#undef AOT_TYPED_DECLARE_BUILTIN_FAMILY
};

enum class InlineBuiltin : uint8_t {
#define AOT_TYPED_DECLARE_INLINE_BUILTIN(name, family, isExact) name,
    FOR_EACH_AOT_TYPED_INLINE_BUILTIN(AOT_TYPED_DECLARE_INLINE_BUILTIN)
#undef AOT_TYPED_DECLARE_INLINE_BUILTIN
};

// The family that lowers it.
inline BuiltinFamily familyOf(InlineBuiltin which)
{
    switch (which) {
#define AOT_TYPED_FAMILY_OF_INLINE_BUILTIN(name, family, isExact) \
    case InlineBuiltin::name: \
        return BuiltinFamily::family;
    FOR_EACH_AOT_TYPED_INLINE_BUILTIN(AOT_TYPED_FAMILY_OF_INLINE_BUILTIN)
#undef AOT_TYPED_FAMILY_OF_INLINE_BUILTIN
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// The code in place does what the call by name does whatever has become of the receiver.
inline bool isExactForEveryReceiver(InlineBuiltin which)
{
    switch (which) {
#define AOT_TYPED_IS_EXACT_INLINE_BUILTIN(name, family, isExact) \
    case InlineBuiltin::name: \
        return isExact;
    FOR_EACH_AOT_TYPED_INLINE_BUILTIN(AOT_TYPED_IS_EXACT_INLINE_BUILTIN)
#undef AOT_TYPED_IS_EXACT_INLINE_BUILTIN
    }
    RELEASE_ASSERT_NOT_REACHED();
}

enum class LanguageOperation : uint8_t {
#define AOT_TYPED_DECLARE_LANGUAGE_OPERATION(name) name,
    FOR_EACH_AOT_TYPED_LANGUAGE_OPERATION(AOT_TYPED_DECLARE_LANGUAGE_OPERATION)
#undef AOT_TYPED_DECLARE_LANGUAGE_OPERATION
};

// A function of the front end's prelude (src/aot/prelude in Bun), which is compiled with the program.
enum class PreludeFunction : uint8_t {
#define AOT_TYPED_DECLARE_PRELUDE_FUNCTION(name, text) name,
    FOR_EACH_AOT_TYPED_PRELUDE_FUNCTION(AOT_TYPED_DECLARE_PRELUDE_FUNCTION)
#undef AOT_TYPED_DECLARE_PRELUDE_FUNCTION
};

static constexpr unsigned numberOfPreludeFunctions = 0
#define AOT_TYPED_COUNT_PRELUDE_FUNCTION(name, text) + 1
    FOR_EACH_AOT_TYPED_PRELUDE_FUNCTION(AOT_TYPED_COUNT_PRELUDE_FUNCTION)
#undef AOT_TYPED_COUNT_PRELUDE_FUNCTION
    ;

static constexpr bool leavesNoFrameInStackTraces(PreludeFunction which) { return which == PreludeFunction::ArrayFlatMap; }
uint32_t visibilityInEngine(uint32_t builtinFunction);
bool isStrictInEngine(uint32_t builtinFunction);

// What the engine's function is. Builtins::findCell() asserts it, and Builtins::mismatches() lists where it is something else.
enum class BuiltinCallee : uint8_t {
    // There is no function: a number, a symbol, an operation, an own property of every instance (the length of an array).
    None,
    // Any function: it is called through Stub::Call.
    Function,
    HostFunction,
    InternalFunction,
};

struct BuiltinImplementation {
    enum Kind : uint8_t {
        // The site is not resolved.
        Generic,
        Call,
        Inline,
        // Builtins::numberOf() of the entry.
        Number,
        // The value is Builtins::cellOf() of the entry.
        Cell,
        Operation,
        Prelude,
    };

    Kind kind { Generic };
    // Also of Inline, for arguments it has no code for, and of Prelude, where the program has no such function.
    BuiltinCallee callee { BuiltinCallee::Function };
    InlineBuiltin inlined { InlineBuiltin::This };
    LanguageOperation operation { LanguageOperation::CopyDataProperties };
    PreludeFunction prelude { };
};

struct Builtin {
    BuiltinKind kind;
    ASCIILiteral holder;
    ASCIILiteral name;
    BuiltinName holderName;
    BuiltinName nameName;
    BuiltinParameters parameters;
    BuiltinResult result;
    uint16_t effects;
    uint16_t usesOfThis;
    BuiltinImplementation implementation;
    // The holder has the function under the private name of the same spelling too.
    bool hasPrivateTwin { false };

    bool callsUserCode() const { return effects & Effect::CallsUserCode; }
    bool takesReceiver() const { return kind == BuiltinKind::Method || kind == BuiltinKind::Getter || kind == BuiltinKind::Setter; }
    unsigned numberOfRequiredParameters() const;
    bool takes(unsigned numberOfArguments) const;
    uint16_t usesOfArgument(unsigned index) const;
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
    // Array is the one class that has two. ArrayType says that an array is as the engine makes them: see JSObject::becomeDerivedArray().
    // An heir of Array is an array as well, and code that is not typed may change any array. So what is TAKEN for an array (a cast, a
    // claim, a store from code that is not typed), and what HOLDS of it as long as it lives, is no more than this:
    // - it is a cell of ArrayType or DerivedArrayType whose ClassInfo is JSArray's, so it is laid out as a JSArray;
    // - it has a butterfly with an indexing header, so the length is where it is for every array;
    // - if it is of DerivedArrayType, its elements are in ArrayStorage.
    JSType type { CellType };
    // Among Builtins::cellOfOriginalStructure(): the structures the engine gives a new instance.
    uint8_t firstOriginalStructure { 0 };
    uint8_t numberOfOriginalStructures { 0 };

    bool canCheckOriginalStructure() const { return type == ArrayType || numberOfOriginalStructures; }
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

struct BuiltinNameData {
    static constexpr uint16_t noClass = UINT16_MAX;
    static constexpr uint16_t severalClasses = UINT16_MAX - 1;

    ASCIILiteral text;
    uint16_t engineClass;
    uint16_t classSpeculatedFor;
    bool isGlobal;
    bool isReplaceable;
    bool mayBeExtended;
};

extern const Builtin builtinEntries[numberOfBuiltins];
// In ascending order of their bytes.
extern const BuiltinNameData builtinNames[numberOfBuiltinNames];
extern const EngineClass engineClasses[numberOfEngineClasses];

class Builtins {
    WTF_MAKE_NONCOPYABLE(Builtins);
public:
    constexpr Builtins() = default;
    JS_EXPORT_PRIVATE static const Builtins& singleton();

    const Builtin& at(BuiltinID id) const { return builtinEntries[id.index()]; }
    unsigned size() const { return numberOfBuiltins; }
    // Of an entry that is a Number.
    double numberOf(BuiltinID) const;

    BuiltinName nameOf(std::span<const uint8_t>) const;

    // The entries of one built-in are adjacent. A lookup yields the first. No holder: the global object.
    BuiltinID find(BuiltinKind, BuiltinName holder, BuiltinName name) const;
    BuiltinID next(BuiltinID) const;
    // Whether the global object has an immutable property of this name that the table has entries for.
    bool isGlobal(BuiltinName name) const { return name && builtinNames[name.index()].isGlobal; }
    // Hosts and programs replace this global object (console) and each of its properties. isGlobal() does not hold: compiled code reads
    // both every time.
    bool isReplaceable(BuiltinName global) const { return global && builtinNames[global.index()].isReplaceable; }
    // Programs extend this class: see Program::extendedGlobals.
    bool mayBeExtended(BuiltinName global) const { return global && builtinNames[global.index()].mayBeExtended; }
    BuiltinID findStatic(BuiltinKind, BuiltinName global, BuiltinName name) const;
    // What an instance with its original structure finds under the name: of its class, of what that extends, of Object.
    BuiltinID findForInstance(BuiltinKind, const EngineClass&, BuiltinName) const;
    BuiltinID findConstructor(BuiltinName global) const;
    const EngineClass* classNamed(BuiltinName) const;
    std::span<const EngineClass> classes() const { return engineClasses; }
    GlobalInterface interfaceNamed(BuiltinName) const;
    BuiltinName prototypeName() const { return BuiltinName(AOT_TYPED_BUILTIN_NAME_prototype); }
    BuiltinName callName() const { return BuiltinName(AOT_TYPED_BUILTIN_NAME_call); }
    BuiltinName applyName() const { return BuiltinName(AOT_TYPED_BUILTIN_NAME_apply); }
    // Function.prototype.call or Function.prototype.apply, if that is the name.
    BuiltinID callOrApply(BuiltinName name) const { return name && (name == callName() || name == applyName()) ? find(BuiltinKind::Method, functionClass().name, name) : BuiltinID(); }
    // Object.prototype.hasOwnProperty, if that is the name.
    BuiltinID hasOwnProperty(BuiltinName name) const { return name == BuiltinName(AOT_TYPED_BUILTIN_NAME_hasOwnProperty) ? find(BuiltinKind::Method, objectClass().name, name) : BuiltinID(); }
    // With `this` for its first argument Object.hasOwn does what Object.prototype.hasOwnProperty does, but that it converts the object first and
    // the key second. Where the conversion of the key runs no code and cannot throw, as its entries ask, that cannot be told.
    BuiltinID hasOwn() const { return findStatic(BuiltinKind::StaticFunction, objectClass().name, BuiltinName(AOT_TYPED_BUILTIN_NAME_hasOwn)); }
    // The only class with a method of that name among those whose instances can be told by one test (typed arrays share their methods and
    // do not count). A receiver of unknown type is speculated to be of it.
    const EngineClass* classSpeculatedFor(BuiltinName method) const;
    std::optional<PreludeFunction> preludeFunctionNamed(std::span<const uint8_t>) const;
    BuiltinID entryCarriedOutBy(PreludeFunction) const;
    bool isHostFunctionInEngine(BuiltinID) const;
    // The operations that passes create or rewrite.
    BuiltinID hasOriginalStructureOperation() const { return BuiltinID(AOT_TYPED_BUILTIN_HasOriginalStructure); }
    BuiltinID callIntrinsic() const { return BuiltinID(AOT_TYPED_BUILTIN_Call); }

    // The cells of the engine that compiled code asks for, by number: Instance::typedBuiltins. The collector does not visit that: the global
    // object keeps each of them for as long as it lives. A function, an accessor, a symbol or a global is reached from it by own
    // properties that cannot be replaced or deleted and, for what the typed arrays share, by a prototype that cannot be changed: lookUp()
    // tests every step. An original structure is a member of JSGlobalObject that is never assigned again. The functions of replaceable
    // globals are the exception: see captureCell().
    static constexpr unsigned numberOfCells() { return numberOfBuiltins + numberOfBuiltinNames + numberOfOriginalStructures; }
    // The function the entry describes: a method, a static function, a getter or setter. Of a constant: the symbol.
    unsigned cellOf(BuiltinID id) const { return id.index(); }
    bool hasCell(BuiltinID id) const { return hasCell(cellOf(id)); }
    bool hasCellAt(unsigned index) const { return hasCell(index); }
    bool isCellOfReplaceableGlobal(unsigned index) const { return index < numberOfBuiltins && isReplaceable(at(BuiltinID(index)).holderName); }
    // What the global object has under the name: isGlobal() holds for it.
    unsigned cellOf(BuiltinName global) const { return numberOfBuiltins + global.index(); }
    unsigned cellOfOriginalStructure(unsigned index) const { return numberOfBuiltins + numberOfBuiltinNames + index; }
    JSCell* findCell(JSGlobalObject*, unsigned) const;
    unsigned cellOfGlobal(ASCIILiteral) const;
    unsigned cellOfStaticFunction(ASCIILiteral global, ASCIILiteral name) const;
    // Compiled code does not test whether these are there.
    std::array<unsigned, 2> cellsFilledFromStart() const { return { cellOf(callOrApply(callName())), cellOf(callOrApply(applyName())) }; }
    // The cells of the functions of replaceable globals. Those of one object are filled in when the host makes the object, which is before
    // any program can replace a function of it, and whoever does that keeps them alive: didCreateReplaceableGlobal(). What is filled in
    // where the object has not what the table says is a cell that no value is, and so is what findCell() yields if it was never made.
    std::span<const uint16_t> cellsOfReplaceableGlobals() const;
    JSCell* captureCell(JSGlobalObject*, JSObject* holder, unsigned) const;
    // One line for each cell that findCell() would trap on, and for each number of the table that the realm has another value for.
    JS_EXPORT_PRIVATE Vector<String> mismatches(JSGlobalObject*) const;

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
#define AOT_TYPED_DECLARE_CLASS_GETTER(lower, upper) const EngineClass& lower ## Class() const { return engineClasses[AOT_TYPED_ENGINE_CLASS_ ## upper]; }
    FOR_EACH_AOT_TYPED_WELL_KNOWN_CLASS(AOT_TYPED_DECLARE_CLASS_GETTER)
#undef AOT_TYPED_DECLARE_CLASS_GETTER

private:
    bool hasCell(unsigned) const;
    std::expected<JSValue, String> findProperty(JSGlobalObject*, const Builtin&, JSObject* holder = nullptr, bool underPrivateName = false) const;
    std::expected<JSCell*, String> findOriginalStructure(JSGlobalObject*, unsigned) const;
    std::expected<JSCell*, String> lookUp(JSGlobalObject*, unsigned, JSObject* holder = nullptr) const;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
