/*
 * Copyright (C) 2015-2022 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include "JSGlobalObject.h"
#include "JSObject.h"
#include "PropertyDescriptor.h"

namespace JSC {

inline constexpr ASCIILiteral ReflectOwnKeysNonObjectArgumentError { "Reflect.ownKeys requires the first argument be an object"_s };

JSC_DECLARE_HOST_FUNCTION(reflectObjectConstruct);
JSC_DECLARE_HOST_FUNCTION(reflectObjectOwnKeys);

// A locked property (Structure::isImmutableProperty()) refuses every store, and reflection may say so.
// The structure keeps its attributes, for shadowing, [[DefineOwnProperty]] and the invariants of proxies.
JS_EXPORT_PRIVATE bool reportIfLocked(JSObject* holder, PropertyName, PropertyDescriptor&);

// JSObject::getOwnPropertyDescriptor() inlined, then the test for a lock.
ALWAYS_INLINE bool getReportedDescriptor(JSGlobalObject* globalObject, JSObject* object, PropertyName propertyName, PropertyDescriptor& descriptor)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PropertySlot slot(object, PropertySlot::InternalMethodType::GetOwnProperty);
    bool isOwn = object->methodTable()->getOwnPropertySlot(object, globalObject, propertyName, slot);
    EXCEPTION_ASSERT_UNUSED(scope, !scope.exception() || !isOwn);
    if (!isOwn)
        return false;
    // The caller checks for an exception.
    scope.release();
    bool isDescribed = descriptor.setPropertySlot(globalObject, propertyName, slot);
    // Proxies without a trap answer with their target's slot. The characters of a string object have no slot base.
    JSObject* holder = slot.slotBase() ? slot.slotBase() : object;
    if (holder->structure()->mayHaveImmutableProperties()) [[unlikely]]
        reportIfLocked(holder, propertyName, descriptor);
    return isDescribed;
}

class ReflectObject final : public JSNonFinalObject {
public:
    using Base = JSNonFinalObject;
    static constexpr unsigned StructureFlags = Base::StructureFlags | HasStaticPropertyTable;

    template<typename CellType, SubspaceAccess>
    static GCClient::IsoSubspace* subspaceFor(VM& vm)
    {
        STATIC_ASSERT_ISO_SUBSPACE_SHARABLE(ReflectObject, Base);
        return &vm.plainObjectSpace();
    }

    static ReflectObject* create(VM& vm, JSGlobalObject* globalObject, Structure* structure)
    {
        ReflectObject* object = new (NotNull, allocateCell<ReflectObject>(vm)) ReflectObject(vm, structure);
        object->finishCreation(vm, globalObject);
        return object;
    }

    DECLARE_INFO;

    inline static Structure* createStructure(VM&, JSGlobalObject*, JSValue);

private:
    ReflectObject(VM&, Structure*);
    void finishCreation(VM&, JSGlobalObject*);
};

} // namespace JSC
