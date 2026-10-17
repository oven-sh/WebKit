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
static constexpr uint16_t RetainsContents = 1 << 6;
static constexpr uint16_t EscapesToUserCode = 1 << 7;
static constexpr uint16_t AppliesTransitively = 1 << 8;
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

struct BuiltinResult {
    static constexpr uint16_t Undefined = 1 << 0;
    static constexpr uint16_t Null = 1 << 1;
    static constexpr uint16_t Boolean = 1 << 2;
    static constexpr uint16_t Integer = 1 << 3;
    static constexpr uint16_t Number = 1 << 4;
    static constexpr uint16_t String = 1 << 5;
    static constexpr uint16_t Symbol = 1 << 6;
    static constexpr uint16_t BigInt = 1 << 7;
    static constexpr uint16_t Instance = 1 << 8;
    static constexpr uint16_t InstanceOfSameClass = 1 << 9;
    static constexpr uint16_t This = 1 << 10;

    uint16_t kinds { 0 };
    uint16_t engineClass { 0 };
    uint8_t arguments { 0 };
    int64_t minimum { 0 };
    int64_t maximum { 0 };

    bool isAnything() const { return !kinds && !arguments; }
};

namespace Effect {
static constexpr uint16_t Pure = 0;
static constexpr uint16_t MayThrow = 1 << 0;
static constexpr uint16_t Allocates = 1 << 1;
static constexpr uint16_t ReadsReceiver = 1 << 2;
static constexpr uint16_t WritesReceiver = 1 << 3;
static constexpr uint16_t ReadsArguments = 1 << 4;
static constexpr uint16_t WritesArguments = 1 << 5;
static constexpr uint16_t ReadsRuntimeState = 1 << 6;
static constexpr uint16_t WritesRuntimeState = 1 << 7;
static constexpr uint16_t CallsUserCode = 1 << 8;
static constexpr uint16_t ClobbersWorld = CallsUserCode | MayThrow | Allocates | ReadsReceiver | WritesReceiver | ReadsArguments | WritesArguments | ReadsRuntimeState | WritesRuntimeState;
}

enum class BuiltinFamily : uint8_t {
#define AOT_TYPED_DECLARE_BUILTIN_FAMILY(name) name,
    FOR_EACH_AOT_TYPED_BUILTIN_FAMILY(AOT_TYPED_DECLARE_BUILTIN_FAMILY)
#undef AOT_TYPED_DECLARE_BUILTIN_FAMILY
};

enum class InlineBuiltin : uint16_t {
#define AOT_TYPED_DECLARE_INLINE_BUILTIN(name, family, isExact) name,
    FOR_EACH_AOT_TYPED_INLINE_BUILTIN(AOT_TYPED_DECLARE_INLINE_BUILTIN)
#undef AOT_TYPED_DECLARE_INLINE_BUILTIN
};

inline BuiltinFamily familyOf(InlineBuiltin which)
{
    switch (which) {
#define AOT_TYPED_INLINE_BUILTIN_FAMILY(name, family, isExact) \
    case InlineBuiltin::name: \
        return BuiltinFamily::family;
    FOR_EACH_AOT_TYPED_INLINE_BUILTIN(AOT_TYPED_INLINE_BUILTIN_FAMILY)
#undef AOT_TYPED_INLINE_BUILTIN_FAMILY
    }
    RELEASE_ASSERT_NOT_REACHED();
}

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

static constexpr bool isHiddenFromStackTraces(PreludeFunction which) { return which == PreludeFunction::ArrayFlatMap; }
static constexpr bool isSelectedAtCallSite(PreludeFunction which) { return which == PreludeFunction::ArrayFilterTruthy || which == PreludeFunction::ArrayMapByNumber || which == PreludeFunction::ArrayMapJoin; }
uint32_t visibilityInEngine(uint32_t builtinFunction);
bool isStrictInEngine(uint32_t builtinFunction);

enum class BuiltinCallee : uint8_t {
    None,
    Function,
    HostFunction,
    InternalFunction,
};

struct BuiltinImplementation {
    enum Kind : uint8_t {
        Generic,
        Call,
        Inline,
        Number,
        Cell,
        Operation,
        Prelude,
    };

    Kind kind { Generic };
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
    uint16_t thisUses;
    BuiltinImplementation implementation;
    bool hasPrivateAlias { false };

    bool callsUserCode() const { return effects & Effect::CallsUserCode; }
    bool hasReceiver() const { return kind == BuiltinKind::Method || kind == BuiltinKind::Getter || kind == BuiltinKind::Setter; }
    unsigned numberOfRequiredParameters() const;
    bool takes(unsigned numberOfArguments) const;
    uint16_t argumentUses(unsigned index) const;
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
    BuiltinName listedAs;
    TypedArrayType typedArrayType { NotTypedArray };
    JSType type { CellType };
    uint8_t firstOriginalStructure { 0 };
    uint8_t numberOfOriginalStructures { 0 };

    bool canCheckOriginalStructure() const { return type == ArrayType || numberOfOriginalStructures; }
};

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
    static constexpr uint16_t multipleClasses = UINT16_MAX - 1;
    static constexpr uint16_t noOperation = UINT16_MAX;

    ASCIILiteral text;
    uint16_t engineClass;
    uint16_t classSpeculatedFor;
    uint16_t operation;
    bool isGlobal;
    bool isReplaceable;
    bool mayBeExtended;
};

extern const Builtin builtinEntries[numberOfBuiltins];
extern const BuiltinNameData builtinNames[numberOfBuiltinNames];

namespace Operations {
#define AOT_TYPED_DEFINE_OPERATION(name, entry, nameOfEntry) inline constexpr BuiltinID name { entry };
FOR_EACH_AOT_TYPED_IR_OPERATION(AOT_TYPED_DEFINE_OPERATION)
#undef AOT_TYPED_DEFINE_OPERATION
}
extern const EngineClass engineClasses[numberOfEngineClasses];

class Builtins {
    WTF_MAKE_NONCOPYABLE(Builtins);
public:
    constexpr Builtins() = default;
    JS_EXPORT_PRIVATE static const Builtins& singleton();

    const Builtin& at(BuiltinID id) const { return builtinEntries[id.index()]; }
    unsigned size() const { return numberOfBuiltins; }
    double numberOf(BuiltinID) const;

    BuiltinName nameOf(std::span<const uint8_t>) const;

    BuiltinID find(BuiltinKind, BuiltinName holder, BuiltinName name) const;
    BuiltinID next(BuiltinID) const;
    bool isGlobal(BuiltinName name) const { return name && builtinNames[name.index()].isGlobal; }
    bool isReplaceable(BuiltinName global) const { return global && builtinNames[global.index()].isReplaceable; }
    bool mayBeExtended(BuiltinName global) const { return global && builtinNames[global.index()].mayBeExtended; }
    BuiltinID findStatic(BuiltinKind, BuiltinName global, BuiltinName name) const;
    BuiltinID findForInstance(BuiltinKind, const EngineClass&, BuiltinName) const;
    BuiltinID findConstructor(BuiltinName global) const;
    const EngineClass* classNamed(BuiltinName) const;
    const EngineClass* classWithFixedPrototypeNamed(BuiltinName name) const
    {
        const EngineClass* engineClass = classNamed(name);
        return engineClass && engineClass->type != ErrorInstanceType ? engineClass : nullptr;
    }
    std::span<const EngineClass> classes() const { return engineClasses; }
    GlobalInterface interfaceNamed(BuiltinName) const;
    BuiltinName prototypeName() const { return BuiltinName(AOT_TYPED_BUILTIN_NAME_prototype); }
    BuiltinName callName() const { return BuiltinName(AOT_TYPED_BUILTIN_NAME_call); }
    BuiltinName applyName() const { return BuiltinName(AOT_TYPED_BUILTIN_NAME_apply); }
    BuiltinID callOrApply(BuiltinName name) const { return name && (name == callName() || name == applyName()) ? find(BuiltinKind::Method, functionClass().name, name) : BuiltinID(); }
    BuiltinID hasOwnProperty(BuiltinName name) const { return name == BuiltinName(AOT_TYPED_BUILTIN_NAME_hasOwnProperty) ? find(BuiltinKind::Method, objectClass().name, name) : BuiltinID(); }
    BuiltinID hasOwn() const { return findStatic(BuiltinKind::StaticFunction, objectClass().name, BuiltinName(AOT_TYPED_BUILTIN_NAME_hasOwn)); }
    const EngineClass* classSpeculatedFor(BuiltinName method) const;
    std::optional<PreludeFunction> preludeFunctionNamed(std::span<const uint8_t>) const;
    BuiltinID entryImplementedBy(PreludeFunction) const;
    bool isHostFunctionInEngine(BuiltinID) const;
    BuiltinID operationNamed(BuiltinName name) const { return name ? BuiltinID(builtinNames[name.index()].operation) : BuiltinID(); }
    BuiltinID hasOriginalStructureOperation() const { return BuiltinID(AOT_TYPED_BUILTIN_HasOriginalStructure); }
    BuiltinID callIntrinsic() const { return BuiltinID(AOT_TYPED_BUILTIN_Call); }

    static constexpr unsigned numberOfCells() { return numberOfBuiltins + numberOfBuiltinNames + numberOfOriginalStructures + numberOfEngineClasses; }
    unsigned cellOf(BuiltinID id) const { return id.index(); }
    bool hasCell(BuiltinID id) const { return hasCell(cellOf(id)); }
    bool hasCellAt(unsigned index) const { return hasCell(index); }
    bool isReplaceableGlobalCell(unsigned index) const { return index < numberOfBuiltins ? isReplaceable(at(BuiltinID(index)).holderName) : index < originalStructureCell(0) && isReplaceable(BuiltinName(index - numberOfBuiltins)); }
    unsigned cellOf(BuiltinName global) const { return numberOfBuiltins + global.index(); }
    unsigned originalStructureCell(unsigned index) const { return numberOfBuiltins + numberOfBuiltinNames + index; }
    static constexpr unsigned firstPrototypeCell() { return numberOfBuiltins + numberOfBuiltinNames + numberOfOriginalStructures; }
    unsigned prototypeCell(const EngineClass& engineClass) const { return firstPrototypeCell() + engineClass.index; }
    JSCell* findCell(JSGlobalObject*, unsigned) const;
    unsigned globalCell(ASCIILiteral) const;
    unsigned staticFunctionCell(ASCIILiteral global, ASCIILiteral name) const;
    std::array<unsigned, 2> eagerCells() const { return { cellOf(callOrApply(callName())), cellOf(callOrApply(applyName())) }; }
    std::span<const uint16_t> replaceableGlobalCells() const;
    JSCell* captureCell(JSGlobalObject*, JSObject* holder, unsigned) const;
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
