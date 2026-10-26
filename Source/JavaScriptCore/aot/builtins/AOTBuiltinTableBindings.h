/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinIDTypes.h"
#include "JSExportMacros.h"
#include "NativeFunction.h"
#include "PropertySlot.h"
#include <optional>
#include <span>

namespace WTF {
class UniquedStringImpl;
}

namespace JSC {

class JSCell;
class JSGlobalObject;
class JSObject;
class Structure;
struct ClassInfo;
enum class LinkTimeConstant : int32_t;

namespace AOT {

class BuiltinTable;
enum class BuiltinOrigin : uint8_t;

struct BuiltinClassBinding {
    BuiltinClassID builtinClass { };
    const ClassInfo* instanceClassInfo { nullptr };
    JSObject* (*findPrototype)(JSGlobalObject*) { nullptr };
    Structure* (*findInstanceStructure)(JSGlobalObject*) { nullptr };
};

struct BuiltinCachedGetterBinding {
    BuiltinOverloadID overload { };
    uint32_t offsetOfCachedValue { 0 };
};

struct BuiltinCustomGetterBinding {
    BuiltinOverloadID overload { };
    GetValueFunc::Ptr function { nullptr };
};

enum class BuiltinDirectValue : uint8_t {
    Realm,
    NativeObjectOfReceiver,
    Receiver,
    Value,
    ValueOrEmpty,
    RestOfValues,
    FrameOfValues,
    Boolean,
    Int32,
    Double,
    // An address and a count, in two registers. The function has the memory until it returns, and never the cell.
    Bytes,
    BytesOfReceiver,
    // A part of a view. Where it starts and ends, in bytes, are the last arguments of the call: two for each such parameter, in their order.
    BytesInRange,
    BytesOfReceiverInRange,
    Characters,
};

// Set in the address of characters that have 16 bits each.
inline constexpr uintptr_t wideCharactersBit = static_cast<uintptr_t>(1) << 63;

// What a function with returnsFields answers: the fields of the layout that its row returns.
struct BuiltinDirectFields {
    uint64_t first;
    uint64_t second;
};

struct BuiltinDirectFunctionBinding {
    static constexpr unsigned maxParameterCount = 6;

    static constexpr bool takesReceiver(BuiltinDirectValue parameter) { return parameter == BuiltinDirectValue::NativeObjectOfReceiver || parameter == BuiltinDirectValue::Receiver || parameter == BuiltinDirectValue::BytesOfReceiver || parameter == BuiltinDirectValue::BytesOfReceiverInRange || parameter == BuiltinDirectValue::FrameOfValues; }
    static constexpr bool takesBounds(BuiltinDirectValue parameter) { return parameter == BuiltinDirectValue::BytesInRange || parameter == BuiltinDirectValue::BytesOfReceiverInRange; }
    static constexpr bool takesArgument(BuiltinDirectValue parameter) { return parameter == BuiltinDirectValue::FrameOfValues || (parameter != BuiltinDirectValue::Realm && !takesReceiver(parameter)); }
    bool takesReceiver() const { return std::ranges::any_of(std::span(parameters).first(parameterCount), [](BuiltinDirectValue parameter) { return takesReceiver(parameter); }); }
    bool takesFrame() const { return std::ranges::contains(std::span(parameters).first(parameterCount), BuiltinDirectValue::FrameOfValues); }
    bool entersHostFunction() const { return parameterCount == 2 && parameters[0] == BuiltinDirectValue::Realm && parameters[1] == BuiltinDirectValue::FrameOfValues; }
    size_t numberOfBounds() const { return 2 * std::ranges::count_if(std::span(parameters).first(parameterCount), [](BuiltinDirectValue parameter) { return takesBounds(parameter); }); }
    size_t minArgumentCount() const
    {
        return numberOfBounds() + std::ranges::count_if(std::span(parameters).first(parameterCount), [](BuiltinDirectValue parameter) {
            return takesArgument(parameter) && parameter != BuiltinDirectValue::RestOfValues && parameter != BuiltinDirectValue::FrameOfValues && parameter != BuiltinDirectValue::ValueOrEmpty;
        });
    }

    BuiltinOverloadID overload { };
    const void* function { nullptr };
    std::array<BuiltinDirectValue, maxParameterCount> parameters { };
    uint8_t parameterCount { 0 };
    bool returnsFields { false };
    uint16_t offsetOfNativeObject { 0 };
};

struct BuiltinNativeFunctionBinding {
    BuiltinID builtin { };
    NativeFunction::Ptr function { nullptr };
    unsigned length { 0 };
    ASCIILiteral name { };
};

struct BuiltinHostNumberBinding {
    BuiltinID builtin { };
    uint16_t number { 0 };
};

struct BuiltinBindingSet {
    std::span<const BuiltinClassBinding> classes { };
    std::span<const BuiltinCachedGetterBinding> cachedGetters { };
    std::span<const BuiltinCustomGetterBinding> customGetters { };
    std::span<const BuiltinDirectFunctionBinding> directFunctions { };
    std::span<const BuiltinNativeFunctionBinding> nativeFunctions { };
    std::span<const BuiltinHostNumberBinding> hostNumbers { };
};

struct EngineClassBinding {
    NamedBuiltinClass builtinClass;
    const ClassInfo* instanceClassInfo { nullptr };
};

struct EngineNativeFunctionBinding {
    NamedBuiltin builtin;
    NativeFunction::Ptr function { nullptr };
    unsigned length { 0 };
    ASCIILiteral name { };
};

struct EngineLinkTimeConstantBinding {
    NamedBuiltin builtin;
    LinkTimeConstant constant { };
};

struct EngineBindingSet {
    std::span<const EngineClassBinding> classes { };
    std::span<const EngineNativeFunctionBinding> nativeFunctions { };
    std::span<const EngineLinkTimeConstantBinding> linkTimeConstants { };
};

extern const EngineBindingSet engineBuiltinBindings;

struct BuiltinNativeFunction {
    NativeFunction::Ptr function { nullptr };
    unsigned length { 0 };
    // Of the function object. A stack trace lists the function under it, and lists none that has no name here.
    ASCIILiteral name { };
};

class BuiltinBindings {
public:
    JS_EXPORT_PRIVATE static void registerHost(const BuiltinBindingSet&);
    JS_EXPORT_PRIVATE static bool hasHost();
    static bool provides(BuiltinOrigin);

    static const ClassInfo* instanceClassInfoOf(BuiltinClassID);
    static void fillInstanceClassInfos(std::span<const ClassInfo*> infosByClass);
    static BuiltinClassID classOf(const ClassInfo*);
    static JSObject* findPrototype(JSGlobalObject*, BuiltinClassID);
    static std::span<const BuiltinClassBinding> classesOfHost();
    static std::optional<uint32_t> cachedValueOffsetOf(BuiltinOverloadID);
    static std::optional<uint32_t> cachedValueOffsetOf(const ClassInfo*, WTF::UniquedStringImpl* getterName);
    static GetValueFunc::Ptr customGetterOf(BuiltinOverloadID);
    JS_EXPORT_PRIVATE static std::optional<uint16_t> hostNumberOf(BuiltinID);
    static std::optional<uint32_t> directFunctionNumberOf(BuiltinOverloadID);
    static std::optional<uint32_t> directFunctionNumberOf(BuiltinOverloadID, size_t argumentCount);
    static const BuiltinDirectFunctionBinding& directFunctionAt(uint32_t number);
    static std::span<const BuiltinDirectFunctionBinding> directFunctions();
    static std::optional<BuiltinNativeFunction> nativeFunctionOf(BuiltinID);
    static std::optional<uint32_t> nativeFunctionNumberOf(BuiltinID);
    static std::span<const BuiltinNativeFunctionBinding> nativeFunctionsOfHost();
    static std::optional<LinkTimeConstant> linkTimeConstantOf(BuiltinID);
};

// Nothing where the class has no test, or its test asks for a ClassInfo that nothing has bound.
std::optional<bool> passesInstanceTest(const BuiltinTable&, BuiltinClassID, JSCell*);

} // namespace AOT
} // namespace JSC

#endif // ENABLE(AOT)
