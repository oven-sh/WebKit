/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTBuiltinTableBindings.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "ClassInfo.h"
#include <algorithm>
#include <atomic>

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
        && std::ranges::is_sorted(set.appendFunctions, { }, &BuiltinAppendFunctionBinding::builtin)
        && std::ranges::is_sorted(set.nativeFunctions, { }, &BuiltinNativeFunctionBinding::builtin)
        && std::ranges::is_sorted(set.linkTimeConstants, { }, &BuiltinLinkTimeConstantBinding::builtin);
}
#endif

void BuiltinBindings::registerHost(const BuiltinBindingSet& set)
{
    RELEASE_ASSERT(!hasHost());
    ASSERT(hasSortedLists(engineBuiltinBindings));
    ASSERT(hasSortedLists(set));
    s_host.store(&set, std::memory_order_release);
}

bool BuiltinBindings::hasHost()
{
    return !!registeredHost();
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
    if (const Binding* binding = findBindingIn(engineBuiltinBindings.*list, key, id))
        return binding;
    const BuiltinBindingSet* host = registeredHost();
    return host ? findBindingIn(host->*list, key, id) : nullptr;
}

const ClassInfo* BuiltinBindings::instanceClassInfoOf(BuiltinClassID id)
{
    auto* binding = findBinding(&BuiltinBindingSet::classes, &BuiltinClassBinding::builtinClass, id);
    return binding ? binding->instanceClassInfo : nullptr;
}

static BuiltinClassID findClassWithInfo(const BuiltinBindingSet& set, const ClassInfo* info)
{
    auto found = std::ranges::find(set.classes, info, &BuiltinClassBinding::instanceClassInfo);
    return found == set.classes.end() ? BuiltinClassID() : found->builtinClass;
}

BuiltinClassID BuiltinBindings::classOf(const ClassInfo* info)
{
    const BuiltinBindingSet* host = registeredHost();
    for (; info; info = info->parentClass) {
        if (BuiltinClassID found = findClassWithInfo(engineBuiltinBindings, info))
            return found;
        if (!host)
            continue;
        if (BuiltinClassID found = findClassWithInfo(*host, info))
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
        if (table.at(overload).mode != BuiltinCallMode::Get)
            continue;
        if (std::optional<uint32_t> offset = cachedValueOffsetOf(overload))
            return offset;
    }
    return std::nullopt;
}

const void* BuiltinBindings::appendFunctionOf(BuiltinID id)
{
    auto* binding = findBinding(&BuiltinBindingSet::appendFunctions, &BuiltinAppendFunctionBinding::builtin, id);
    return binding ? binding->function : nullptr;
}

const BuiltinNativeFunctionBinding* BuiltinBindings::nativeFunctionOf(BuiltinID id)
{
    return findBinding(&BuiltinBindingSet::nativeFunctions, &BuiltinNativeFunctionBinding::builtin, id);
}

std::optional<LinkTimeConstant> BuiltinBindings::linkTimeConstantOf(BuiltinID id)
{
    auto* binding = findBinding(&BuiltinBindingSet::linkTimeConstants, &BuiltinLinkTimeConstantBinding::builtin, id);
    if (!binding)
        return std::nullopt;
    return binding->constant;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
