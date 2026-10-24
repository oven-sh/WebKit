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

// One function stands in for all the host functions of a feature. The function object in the frame has the name under which the program knows the one that it called.
static NEVER_INLINE EncodedJSValue throwForRemovedCallee(JSGlobalObject* globalObject, CallFrame* callFrame, uint16_t feature)
{
    String name;
    // The text of a feature of the host says where the function is found: Bun.color.
    if (CalleeBits callee = callFrame->callee(); feature < firstEmbedderRemovableFeature && callee.isCell()) {
        if (auto* function = dynamicDowncast<JSFunction>(callee.asCell()))
            name = function->name(globalObject->vm());
        else if (auto* function = dynamicDowncast<InternalFunction>(callee.asCell()))
            name = function->name();
    }
    return Bun__throwForRemovedFunction(globalObject, feature, name.impl());
}

template<uint16_t feature>
static EncodedJSValue JSC_HOST_CALL_ATTRIBUTES throwForRemovedHostFunction(JSGlobalObject* globalObject, CallFrame* callFrame)
{
    return throwForRemovedCallee(globalObject, callFrame, feature);
}

template<>
JS_EXPORT_PRIVATE NativeFunction functionUnlessRemovedSlow(NativeFunction function)
{
    static constexpr auto functions = []<size_t... features>(std::index_sequence<features...>) {
        return std::array<NativeFunction::Ptr, sizeof...(features)> { &throwForRemovedHostFunction<features>... };
    }(std::make_index_sequence<removableFeatureIndexLimit>());
    std::optional<RemovableFeature> feature = removedFeatureAt(function.untaggedPtr());
    return feature ? NativeFunction(functions[static_cast<uint16_t>(*feature)]) : function;
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

static void throwForRemovedAccessor(JSGlobalObject* globalObject, EncodedJSValue thisValue, PropertyName propertyName, AccessorFunction function)
{
    // The embedder has a text for an index that no feature has.
    uint16_t index = removableFeatureIndexLimit;
    for (JSObject* object = JSValue::decode(thisValue).getObject(); object; object = object->getPrototypeDirect().getObject()) {
        unsigned attributes;
        JSValue value = object->getDirect(globalObject->vm(), propertyName, attributes);
        if (!value || !(attributes & PropertyAttribute::CustomAccessorOrValue))
            continue;
        auto* accessor = uncheckedDowncast<CustomGetterSetter>(value);
        if (std::optional<RemovableFeature> feature = function == AccessorFunction::Getter ? accessor->removedGetterFeature() : accessor->removedSetterFeature()) {
            index = static_cast<uint16_t>(*feature);
            break;
        }
    }
    Bun__throwForRemovedFeature(globalObject, index);
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
