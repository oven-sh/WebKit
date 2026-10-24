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

class JSGlobalObject;
class JSObject;
class Structure;
struct ClassInfo;
enum class LinkTimeConstant : int32_t;

namespace AOT {

enum class BuiltinOrigin : uint8_t;

struct BuiltinClassBinding {
    BuiltinClassID builtinClass { };
    const ClassInfo* instanceClassInfo { nullptr };
    JSObject* (*findPrototype)(JSGlobalObject*) { nullptr };
    // Answers null until the realm has created the Structure, and creates nothing.
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

// One parameter of a native function that compiled code calls without a function object and without a frame for the callee.
enum class BuiltinDirectValue : uint8_t {
    // JSGlobalObject*. No operand of the call.
    Realm,
    // void*: what the receiver, an instance of a class of the host, wraps. It lies at offsetOfNativeObject in the cell.
    NativeObjectOfReceiver,
    // EncodedJSValue of the receiver.
    Receiver,
    // The others take the arguments of the call, in order.
    Value,
    // EncodedJSValue, or the empty value where the call has no such argument.
    ValueOrEmpty,
    // const EncodedJSValue*, size_t: this operand and all behind it. The last parameter.
    RestOfValues,
    FrameOfValues,
    // bool.
    Boolean,
    // int32_t.
    Int32,
    // double.
    Double,
};

// The native code behind one overload: EncodedJSValue (parameters ..) in the calling convention of the operations of the run time.
struct BuiltinDirectFunctionBinding {
    static constexpr unsigned maxParameterCount = 6;

    static constexpr bool takesReceiver(BuiltinDirectValue parameter) { return parameter == BuiltinDirectValue::NativeObjectOfReceiver || parameter == BuiltinDirectValue::Receiver || parameter == BuiltinDirectValue::FrameOfValues; }
    static constexpr bool takesArgument(BuiltinDirectValue parameter) { return parameter == BuiltinDirectValue::FrameOfValues || (parameter != BuiltinDirectValue::Realm && !takesReceiver(parameter)); }
    bool takesReceiver() const { return std::ranges::any_of(std::span(parameters).first(parameterCount), [](BuiltinDirectValue parameter) { return takesReceiver(parameter); }); }
    bool takesFrame() const { return std::ranges::contains(std::span(parameters).first(parameterCount), BuiltinDirectValue::FrameOfValues); }
    bool entersHostFunction() const { return parameterCount == 2 && parameters[0] == BuiltinDirectValue::Realm && parameters[1] == BuiltinDirectValue::FrameOfValues; }
    // A call with fewer arguments is not made directly. The function does not see those behind the last that it takes.
    size_t minArgumentCount() const
    {
        return std::ranges::count_if(std::span(parameters).first(parameterCount), [](BuiltinDirectValue parameter) {
            return takesArgument(parameter) && parameter != BuiltinDirectValue::RestOfValues && parameter != BuiltinDirectValue::FrameOfValues && parameter != BuiltinDirectValue::ValueOrEmpty;
        });
    }

    BuiltinOverloadID overload { };
    const void* function { nullptr };
    std::array<BuiltinDirectValue, maxParameterCount> parameters { };
    uint8_t parameterCount { 0 };
    uint16_t offsetOfNativeObject { 0 };
};

struct BuiltinNativeFunctionBinding {
    BuiltinID builtin { };
    NativeFunction::Ptr function { nullptr };
    unsigned length { 0 };
};

// What the hook of the host takes for a native binding. The numbers depend on how the host is built, the table does not.
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

// What the engine binds. The lists are compiled with the engine and the table is not, so they name an entry by its position in the engine's list of names.
struct EngineClassBinding {
    NamedBuiltinClass builtinClass;
    const ClassInfo* instanceClassInfo { nullptr };
};

struct EngineNativeFunctionBinding {
    NamedBuiltin builtin;
    NativeFunction::Ptr function { nullptr };
    unsigned length { 0 };
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
    // None without a host, and for a binding that this build of the host leaves out.
    JS_EXPORT_PRIVATE static std::optional<uint16_t> hostNumberOf(BuiltinID);
    // The number of the overload's native function among those of the host, for directFunctionAt().
    static std::optional<uint32_t> directFunctionNumberOf(BuiltinOverloadID);
    static std::optional<uint32_t> directFunctionNumberOf(BuiltinOverloadID, size_t argumentCount);
    static const BuiltinDirectFunctionBinding& directFunctionAt(uint32_t number);
    static std::span<const BuiltinDirectFunctionBinding> directFunctions();
    static std::optional<BuiltinNativeFunction> nativeFunctionOf(BuiltinID);
    // The number of the built-in's function among nativeFunctionsOfHost().
    static std::optional<uint32_t> nativeFunctionNumberOf(BuiltinID);
    static std::span<const BuiltinNativeFunctionBinding> nativeFunctionsOfHost();
    static std::optional<LinkTimeConstant> linkTimeConstantOf(BuiltinID);
};

} // namespace AOT
} // namespace JSC

#endif // ENABLE(AOT)
