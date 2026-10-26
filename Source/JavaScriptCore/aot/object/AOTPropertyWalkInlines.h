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

ALWAYS_INLINE bool haveSameFieldsInKeyOrder(const JSObject* compiledObject, const JSObject* other)
{
    ASSERT(isCompiledObject(compiledObject) && isCompiledObject(other));
    return compiledObject->structureID() == other->structureID() && !((uncheckedDowncast<Object>(compiledObject)->orderWord() ^ uncheckedDowncast<Object>(other)->orderWord()) & ~orderWordIntegrityBits);
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

// The narrow references of an object in a frame are not read by cellInNarrowSlot().
ALWAYS_INLINE bool isInFrame(const JSObject* compiledObject)
{
    return uncheckedDowncast<Object>(compiledObject)->isInFrame();
}

ALWAYS_INLINE JSCell* cellInNarrowSlot(const JSObject* compiledObject, uint32_t word)
{
    ASSERT(word > undefinedCell32);
    return decodeCell32(compiledObject, word);
}

template<typename Functor>
ALWAYS_INLINE std::optional<Result> tryVisitListedFields(JSObject* object, bool listsSymbols, const Functor& functor)
{
    Object* typed = compiledObjectOrNull(object);
    bool listsEveryKey = typed && (!listsSymbols || !typed->layout().hasFieldsWithSymbolKeys);
    auto fields = listsEveryKey ? typed->fieldsInKeyOrder() : std::nullopt;
    if (!fields) [[unlikely]]
        return std::nullopt;
    unsigned attributes = typed->attributesOfPublicFields();
    for (auto& property : *fields) {
        JSValue value = typed->readListedField(property);
        if (value && functor(OwnProperty { property.name, value, attributes }) == IterationStatus::Done)
            return Result::Stopped;
    }
    return Result::Completed;
}

template<typename Functor>
ALWAYS_INLINE Result forEachOwnPropertyInline(VM& vm, JSObject* object, Filter filter, const Functor& functor)
{
    if (auto result = tryVisitListedFields(object, filter == Filter::StringsAndSymbols, functor)) [[likely]]
        return *result;
    return forEachOwnProperty(vm, object, filter, functor);
}

template<typename Functor>
ALWAYS_INLINE Result forEachOwnPropertyAsCreatedInline(VM& vm, JSObject* object, const Functor& functor)
{
    if (auto result = tryVisitListedFields(object, true, functor)) [[likely]]
        return *result;
    return forEachOwnPropertyAsCreated(vm, object, functor);
}

} } } // namespace JSC::AOT::PropertyWalk

#endif // ENABLE(AOT)
