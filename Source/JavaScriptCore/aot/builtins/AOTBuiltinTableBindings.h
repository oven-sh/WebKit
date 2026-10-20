/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinIDTypes.h"
#include "JSExportMacros.h"
#include "NativeFunction.h"
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

struct BuiltinAppendFunctionBinding {
    BuiltinID builtin { };
    const void* function { nullptr };
};

struct BuiltinNativeFunctionBinding {
    BuiltinID builtin { };
    NativeFunction::Ptr function { nullptr };
    unsigned length { 0 };
};

struct BuiltinLinkTimeConstantBinding {
    BuiltinID builtin { };
    LinkTimeConstant constant { };
};

struct BuiltinBindingSet {
    std::span<const BuiltinClassBinding> classes { };
    std::span<const BuiltinCachedGetterBinding> cachedGetters { };
    std::span<const BuiltinAppendFunctionBinding> appendFunctions { };
    std::span<const BuiltinNativeFunctionBinding> nativeFunctions { };
    std::span<const BuiltinLinkTimeConstantBinding> linkTimeConstants { };
};

extern const BuiltinBindingSet engineBuiltinBindings;

class BuiltinBindings {
public:
    JS_EXPORT_PRIVATE static void registerHost(const BuiltinBindingSet&);
    JS_EXPORT_PRIVATE static bool hasHost();
    static bool provides(BuiltinOrigin);

    static const ClassInfo* instanceClassInfoOf(BuiltinClassID);
    static BuiltinClassID classOf(const ClassInfo*);
    static JSObject* findPrototype(JSGlobalObject*, BuiltinClassID);
    static std::span<const BuiltinClassBinding> classesOfHost();
    static std::optional<uint32_t> cachedValueOffsetOf(BuiltinOverloadID);
    static std::optional<uint32_t> cachedValueOffsetOf(const ClassInfo*, WTF::UniquedStringImpl* getterName);
    static const void* appendFunctionOf(BuiltinID);
    static const BuiltinNativeFunctionBinding* nativeFunctionOf(BuiltinID);
    static std::optional<LinkTimeConstant> linkTimeConstantOf(BuiltinID);
};

} // namespace AOT
} // namespace JSC

#endif // ENABLE(AOT)
