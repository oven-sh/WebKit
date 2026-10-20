/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRemovableFeature.h"

#include "CustomGetterSetter.h"
#include "JSCInlines.h"
#include <array>

namespace JSC {

template<uint16_t feature>
static EncodedJSValue JSC_HOST_CALL_ATTRIBUTES throwForRemovedHostFunction(JSGlobalObject* globalObject, CallFrame*)
{
    return Bun__throwForRemovedFeature(globalObject, feature);
}

static NativeFunction throwingHostFunctionFor(RemovableFeature feature)
{
    static constexpr auto functions = []<size_t... features>(std::index_sequence<features...>) {
        return std::array<NativeFunction::Ptr, sizeof...(features)> { &throwForRemovedHostFunction<features>... };
    }(std::make_index_sequence<removableFeatureIndexLimit>());
    return functions[static_cast<uint16_t>(feature)];
}

template<>
JS_EXPORT_PRIVATE NativeFunction functionUnlessRemovedSlow(NativeFunction function)
{
    std::optional<RemovableFeature> feature = removedFeatureAt(function.untaggedPtr());
    return feature ? throwingHostFunctionFor(*feature) : function;
}

template<>
JS_EXPORT_PRIVATE PutValueFunc functionUnlessRemovedSlow(PutValueFunc function)
{
    return removedFeatureAt(function.untaggedPtr()) ? PutValueFunc(CustomGetterSetter::setRemovedProperty) : function;
}

void CustomGetterSetter::replaceRemovedFunctions()
{
    if (std::optional<RemovableFeature> feature = removedFeatureAt(m_getter.untaggedPtr())) {
        m_getter = getRemovedProperty;
        m_removedGetterFeature = *feature;
    }
    if (std::optional<RemovableFeature> feature = removedFeatureAt(m_setter.untaggedPtr())) {
        m_setter = setRemovedProperty;
        m_removedSetterFeature = *feature;
    }
}

enum class AccessorFunction : bool { Getter, Setter };

// A custom accessor gets its receiver and a custom value gets the object that has it, so the cell is found from there, except when the caller
// chose the receiver (Reflect.get, super, the function of a property descriptor) or the receiver is a proxy.
static std::optional<RemovableFeature> findRemovedAccessor(VM& vm, JSValue thisValue, PropertyName propertyName, AccessorFunction function)
{
    for (JSObject* object = thisValue.getObject(); object; object = object->getPrototypeDirect().getObject()) {
        unsigned attributes;
        JSValue value = object->getDirect(vm, propertyName, attributes);
        if (!value || !(attributes & PropertyAttribute::CustomAccessorOrValue))
            continue;
        auto* accessor = uncheckedDowncast<CustomGetterSetter>(value);
        if (std::optional<RemovableFeature> feature = function == AccessorFunction::Getter ? accessor->removedGetterFeature() : accessor->removedSetterFeature())
            return feature;
    }
    return std::nullopt;
}

static void throwForRemovedAccessor(JSGlobalObject* globalObject, EncodedJSValue thisValue, PropertyName propertyName, AccessorFunction function)
{
    std::optional<RemovableFeature> feature = findRemovedAccessor(globalObject->vm(), JSValue::decode(thisValue), propertyName, function);
    // The embedder has a text for an index that no feature has.
    Bun__throwForRemovedFeature(globalObject, feature ? static_cast<uint16_t>(*feature) : removableFeatureIndexLimit);
}

JSC_DEFINE_CUSTOM_GETTER(CustomGetterSetter::getRemovedProperty, (JSGlobalObject* globalObject, EncodedJSValue thisValue, PropertyName propertyName))
{
    throwForRemovedAccessor(globalObject, thisValue, propertyName, AccessorFunction::Getter);
    return { };
}

JSC_DEFINE_CUSTOM_SETTER(CustomGetterSetter::setRemovedProperty, (JSGlobalObject* globalObject, EncodedJSValue thisValue, EncodedJSValue, PropertyName propertyName))
{
    throwForRemovedAccessor(globalObject, thisValue, propertyName, AccessorFunction::Setter);
    return false;
}

} // namespace JSC
