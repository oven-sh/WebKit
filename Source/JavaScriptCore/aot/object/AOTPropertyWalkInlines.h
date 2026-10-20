/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTObject.h"

namespace JSC { namespace AOT { namespace PropertyWalk {

ALWAYS_INLINE std::optional<std::span<const FieldAsProperty>> fieldsInKeyOrder(const JSObject* compiledObject)
{
    ASSERT(isCompiledObject(compiledObject));
    return uncheckedDowncast<Object>(compiledObject)->fieldsInKeyOrder();
}

// Empty for a field that is absent.
ALWAYS_INLINE JSValue readListedField(const JSObject* compiledObject, const FieldAsProperty& property)
{
    return uncheckedDowncast<Object>(compiledObject)->readListedField(property);
}

// As Object::readInlineUnboxed. An empty value also stands for a field that holds undefined and has no storage.
template<typename Functor>
ALWAYS_INLINE auto readListedFieldUnboxed(const JSObject* compiledObject, const FieldAsProperty& property, const Functor& functor)
{
    return uncheckedDowncast<Object>(compiledObject)->readInlineUnboxed(*property.field, functor);
}

template<typename Functor>
ALWAYS_INLINE Result forEachOwnPropertyInline(VM& vm, JSObject* object, Filter filter, const Functor& functor)
{
    Object* typed = compiledObjectOrNull(object);
    bool listsEveryKey = typed && (filter != Filter::StringsAndSymbols || !typed->layout().hasFieldsWithSymbolKeys);
    if (auto fields = listsEveryKey ? typed->fieldsInKeyOrder() : std::nullopt) [[likely]] {
        unsigned attributes = typed->attributesOfPublicFields();
        for (auto& property : *fields) {
            JSValue value = typed->readListedField(property);
            if (value && functor(OwnProperty { property.name, value, attributes }) == IterationStatus::Done)
                return Result::Stopped;
        }
        return Result::Completed;
    }
    return forEachOwnProperty(vm, object, filter, functor);
}

} } } // namespace JSC::AOT::PropertyWalk

#endif // ENABLE(AOT)
