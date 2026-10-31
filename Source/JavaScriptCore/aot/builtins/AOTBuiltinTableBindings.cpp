/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTBuiltinTableBindings.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTNativeHelpers.h"
#include "AOTVector.h"
#include "ClassInfo.h"
#include <algorithm>
#include <atomic>
#include <type_traits>
#include <wtf/NeverDestroyed.h>

namespace JSC { namespace AOT {

static constinit std::atomic<const BuiltinBindingSet*> s_host { nullptr };

static const BuiltinBindingSet* registeredHost()
{
    return s_host.load(std::memory_order_acquire);
}

#if ASSERT_ENABLED
static bool hasSortedLists(const BuiltinBindingSet& set)
{
    return std::ranges::is_sorted(set.classes, { }, &BuiltinClassBinding::builtinClass)
        && std::ranges::is_sorted(set.cachedGetters, { }, &BuiltinCachedGetterBinding::overload)
        && std::ranges::is_sorted(set.customGetters, { }, &BuiltinCustomGetterBinding::overload)
        && std::ranges::is_sorted(set.directFunctions, { }, &BuiltinDirectFunctionBinding::overload)
        && std::ranges::is_sorted(set.nativeFunctions, { }, &BuiltinNativeFunctionBinding::builtin)
        && std::ranges::is_sorted(set.hostNumbers, { }, &BuiltinHostNumberBinding::builtin);
}
#endif

void BuiltinBindings::registerHost(const BuiltinBindingSet& set)
{
    RELEASE_ASSERT(!hasHost());
    ASSERT(hasSortedLists(set));
    s_host.store(&set, std::memory_order_release);
}

bool BuiltinBindings::hasHost()
{
    return !!registeredHost();
}

static Vector<BuiltinDirectFunctionBinding>& nativeHelpersOfEngine()
{
    static NeverDestroyed<Vector<BuiltinDirectFunctionBinding>> helpers;
    return helpers;
}

template<typename... Parameters>
static void bindNativeHelper(BuiltinID helper, EncodedJSValue (SYSV_ABI *function)(Parameters...))
{
    static_assert(sizeof...(Parameters) <= BuiltinDirectFunctionBinding::maxParameterCount);
    for (BuiltinOverloadID overload : BuiltinTable::singleton().overloadIDsOf(helper)) {
        BuiltinDirectFunctionBinding binding { .overload = overload, .function = reinterpret_cast<const void*>(function) };
        ((binding.parameters[binding.parameterCount++] = std::is_same_v<Parameters, JSGlobalObject*> ? BuiltinDirectValue::Realm : BuiltinDirectValue::Value), ...);
        nativeHelpersOfEngine().append(binding);
    }
}

void BuiltinBindings::bindNativeHelpersOfEngine()
{
    RELEASE_ASSERT(!hasHost() && nativeHelpersOfEngine().isEmpty());
    bindNativeHelper(Builtins::CompilerHelpersOwnEntriesFlat, AOT__ownEntriesFlat);
    bindNativeHelper(Builtins::CompilerHelpersDescribeUnlessOrdinary, AOT__describeUnlessOrdinary);
    bindNativeHelper(Builtins::CompilerHelpersDefinePropertiesFrom, AOT__definePropertiesFrom);
    bindNativeHelper(Builtins::CompilerHelpersScanPlainQuery, AOT__scanPlainQuery);
    bindNativeHelper(Builtins::CompilerHelpersFindInPlainQuery, AOT__findInPlainQuery);
    bindNativeHelper(Builtins::CompilerHelpersPlainQueryEntries, AOT__plainQueryEntries);
    bindNativeHelper(Builtins::CompilerHelpersPlainQueryKeys, AOT__plainQueryKeys);
    bindNativeHelper(Builtins::CompilerHelpersPlainQueryValues, AOT__plainQueryValues);
    bindNativeHelper(Builtins::CompilerHelpersScanPlainURL, AOT__scanPlainURL);
    bindNativeHelper(Builtins::CompilerHelpersScanPlainURLWithPlainBase, AOT__scanPlainURLWithPlainBase);
    bindNativeHelper(Builtins::CompilerHelpersScanPlainURLWithBase, AOT__scanPlainURLWithBase);
    bindNativeHelper(Builtins::CompilerHelpersFindPlainQueryValue, AOT__findPlainQueryValue);
    std::ranges::sort(nativeHelpersOfEngine(), { }, &BuiltinDirectFunctionBinding::overload);
}

bool BuiltinBindings::provides(BuiltinOrigin origin)
{
    return origin == BuiltinOrigin::Engine || hasHost();
}

template<typename Binding, typename ID>
static const Binding* findBindingIn(std::span<const Binding> bindings, ID Binding::* key, ID id)
{
    auto found = std::ranges::lower_bound(bindings, id, { }, key);
    if (found == bindings.end() || (*found).*key != id)
        return nullptr;
    return &*found;
}

template<typename Binding, typename ID>
static const Binding* findBinding(std::span<const Binding> BuiltinBindingSet::* list, ID Binding::* key, ID id)
{
    const BuiltinBindingSet* host = registeredHost();
    return host ? findBindingIn(host->*list, key, id) : nullptr;
}

template<typename Binding, typename ID>
static const Binding* findEngineBinding(std::span<const Binding> bindings, NamedTableID<ID> Binding::* key, ID id)
{
    auto found = std::ranges::find_if(bindings, [&](const Binding& binding) {
        return static_cast<ID>(binding.*key) == id;
    });
    return found == bindings.end() ? nullptr : &*found;
}

template<typename ID>
static bool isOfEngine(ID id)
{
    const BuiltinTable& table = BuiltinTable::singleton();
    return table.contains(id) && table.at(id).origin == BuiltinOrigin::Engine;
}

const ClassInfo* BuiltinBindings::instanceClassInfoOf(BuiltinClassID id)
{
    if (isOfEngine(id)) {
        auto* binding = findEngineBinding(engineBuiltinBindings.classes, &EngineClassBinding::builtinClass, id);
        return binding ? binding->instanceClassInfo : nullptr;
    }
    auto* binding = findBinding(&BuiltinBindingSet::classes, &BuiltinClassBinding::builtinClass, id);
    return binding ? binding->instanceClassInfo : nullptr;
}

void BuiltinBindings::fillInstanceClassInfos(std::span<const ClassInfo*> infosByClass)
{
    for (const BuiltinClassBinding& binding : classesOfHost())
        infosByClass[binding.builtinClass.index()] = binding.instanceClassInfo;
    for (const EngineClassBinding& binding : engineBuiltinBindings.classes)
        infosByClass[binding.builtinClass.index()] = binding.instanceClassInfo;
}

template<typename Binding>
static BuiltinClassID findClassWithInfo(std::span<const Binding> classes, const ClassInfo* info)
{
    auto found = std::ranges::find(classes, info, &Binding::instanceClassInfo);
    return found == classes.end() ? BuiltinClassID() : BuiltinClassID(found->builtinClass);
}

BuiltinClassID BuiltinBindings::classOf(const ClassInfo* info)
{
    const BuiltinBindingSet* host = registeredHost();
    for (; info; info = info->parentClass) {
        if (BuiltinClassID found = findClassWithInfo(engineBuiltinBindings.classes, info))
            return found;
        if (!host)
            continue;
        if (BuiltinClassID found = findClassWithInfo(host->classes, info))
            return found;
    }
    return { };
}

JSObject* BuiltinBindings::findPrototype(JSGlobalObject* realm, BuiltinClassID id)
{
    auto* binding = findBinding(&BuiltinBindingSet::classes, &BuiltinClassBinding::builtinClass, id);
    return binding && binding->findPrototype ? binding->findPrototype(realm) : nullptr;
}

std::span<const BuiltinClassBinding> BuiltinBindings::classesOfHost()
{
    const BuiltinBindingSet* host = registeredHost();
    return host ? host->classes : std::span<const BuiltinClassBinding>();
}

std::optional<uint32_t> BuiltinBindings::cachedValueOffsetOf(BuiltinOverloadID id)
{
    auto* binding = findBinding(&BuiltinBindingSet::cachedGetters, &BuiltinCachedGetterBinding::overload, id);
    if (!binding)
        return std::nullopt;
    return binding->offsetOfCachedValue;
}

std::optional<uint32_t> BuiltinBindings::cachedValueOffsetOf(const ClassInfo* info, UniquedStringImpl* getterName)
{
    BuiltinClassID builtinClass = classOf(info);
    if (!builtinClass || instanceClassInfoOf(builtinClass) != info)
        return std::nullopt;
    const BuiltinTable& table = BuiltinTable::singleton();
    BuiltinName name = table.textNameOf(getterName);
    if (!name)
        return std::nullopt;
    BuiltinMemberLookup getter = table.findForInstance(builtinClass, name);
    if (getter.outcome != BuiltinMemberLookup::Outcome::Found)
        return std::nullopt;
    for (BuiltinOverloadID overload : table.overloadIDsOf(getter.member)) {
        if (std::optional<uint32_t> offset = cachedValueOffsetOf(overload))
            return offset;
    }
    return std::nullopt;
}

GetValueFunc::Ptr BuiltinBindings::customGetterOf(BuiltinOverloadID id)
{
    auto* binding = findBinding(&BuiltinBindingSet::customGetters, &BuiltinCustomGetterBinding::overload, id);
    return binding ? binding->function : nullptr;
}

std::optional<uint16_t> BuiltinBindings::hostNumberOf(BuiltinID id)
{
    auto* binding = findBinding(&BuiltinBindingSet::hostNumbers, &BuiltinHostNumberBinding::builtin, id);
    return binding ? std::optional(binding->number) : std::nullopt;
}

std::optional<uint32_t> BuiltinBindings::directFunctionNumberOf(BuiltinOverloadID id)
{
    std::span<const BuiltinDirectFunctionBinding> functions = directFunctions();
    const BuiltinDirectFunctionBinding* binding = findBindingIn(functions, &BuiltinDirectFunctionBinding::overload, id);
    if (!binding)
        return std::nullopt;
    return static_cast<uint32_t>(binding - functions.data());
}

std::optional<uint32_t> BuiltinBindings::directFunctionNumberOf(BuiltinOverloadID id, size_t argumentCount)
{
    std::optional<uint32_t> number = directFunctionNumberOf(id);
    if (!number || argumentCount < directFunctionAt(*number).minArgumentCount())
        return std::nullopt;
    return number;
}

const BuiltinDirectFunctionBinding& BuiltinBindings::directFunctionAt(uint32_t number)
{
    return directFunctions()[number];
}

std::span<const BuiltinDirectFunctionBinding> BuiltinBindings::directFunctions()
{
    const BuiltinBindingSet* host = registeredHost();
    return host ? host->directFunctions : nativeHelpersOfEngine().span();
}

std::optional<BuiltinNativeFunction> BuiltinBindings::nativeFunctionOf(BuiltinID id)
{
    if (isOfEngine(id)) {
        auto* binding = findEngineBinding(engineBuiltinBindings.nativeFunctions, &EngineNativeFunctionBinding::builtin, id);
        return binding ? std::optional(BuiltinNativeFunction { binding->function, binding->length, binding->name }) : std::nullopt;
    }
    auto* binding = findBinding(&BuiltinBindingSet::nativeFunctions, &BuiltinNativeFunctionBinding::builtin, id);
    return binding ? std::optional(BuiltinNativeFunction { binding->function, binding->length, binding->name }) : std::nullopt;
}

std::optional<uint32_t> BuiltinBindings::nativeFunctionNumberOf(BuiltinID id)
{
    const BuiltinBindingSet* host = registeredHost();
    if (!host)
        return std::nullopt;
    const BuiltinNativeFunctionBinding* binding = findBindingIn(host->nativeFunctions, &BuiltinNativeFunctionBinding::builtin, id);
    if (!binding)
        return std::nullopt;
    return static_cast<uint32_t>(binding - host->nativeFunctions.data());
}

std::span<const BuiltinNativeFunctionBinding> BuiltinBindings::nativeFunctionsOfHost()
{
    const BuiltinBindingSet* host = registeredHost();
    return host ? host->nativeFunctions : std::span<const BuiltinNativeFunctionBinding>();
}

std::optional<LinkTimeConstant> BuiltinBindings::linkTimeConstantOf(BuiltinID id)
{
    auto* binding = isOfEngine(id) ? findEngineBinding(engineBuiltinBindings.linkTimeConstants, &EngineLinkTimeConstantBinding::builtin, id) : nullptr;
    if (!binding)
        return std::nullopt;
    return binding->constant;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
