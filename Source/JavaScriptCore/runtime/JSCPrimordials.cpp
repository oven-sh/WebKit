/*
 * Copyright (C) 2019 Apple Inc. All rights reserved.
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

#include "config.h"

#if USE(BUN_JSC_ADDITIONS)

#include "JSCPrimordials.h"

#include "JSCJSValueInlines.h"
#include "JSGlobalObject.h"
#include "JSObjectInlines.h"
#include "LazyPropertyInlines.h"
#include "LinkTimeConstant.h"
#include "PropertyDescriptor.h"
#include "TopExceptionScope.h"

namespace JSC {

static ALWAYS_INLINE JSCell* primordialMethod(JSGlobalObject* globalObject, VM& vm, JSObject* holder, const Identifier& key)
{
    JSValue value = holder->getDirect(vm, key);
    if (value.isEmpty()) {
        auto catchScope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
        value = holder->get(globalObject, key);
        catchScope.releaseAssertNoException();
    }
    RELEASE_ASSERT_WITH_MESSAGE(value.isCell(), "primordial %s is not a cell", key.utf8().data());
    return value.asCell();
}

static ALWAYS_INLINE JSCell* primordialGetter(JSGlobalObject* globalObject, VM& vm, JSObject* holder, const Identifier& key)
{
    auto catchScope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    PropertyDescriptor descriptor;
    bool found = holder->getOwnPropertyDescriptor(globalObject, key, descriptor);
    catchScope.releaseAssertNoException();
    RELEASE_ASSERT_WITH_MESSAGE(found, "primordial getter %s not found", key.utf8().data());
    JSValue getter = descriptor.getter();
    RELEASE_ASSERT_WITH_MESSAGE(getter.isCell(), "primordial getter %s is not a cell", key.utf8().data());
    return getter.asCell();
}

#define JSC_PRIMORDIAL_KEY_Method(key) Identifier::fromString(vm, key##_s)
#define JSC_PRIMORDIAL_KEY_Getter(key) Identifier::fromString(vm, key##_s)
#define JSC_PRIMORDIAL_KEY_SymbolMethod(key) vm.propertyNames->key##Symbol
#define JSC_PRIMORDIAL_KEY_SymbolGetter(key) vm.propertyNames->key##Symbol

#define JSC_PRIMORDIAL_READ_Method primordialMethod
#define JSC_PRIMORDIAL_READ_Getter primordialGetter
#define JSC_PRIMORDIAL_READ_SymbolMethod primordialMethod
#define JSC_PRIMORDIAL_READ_SymbolGetter primordialGetter

#define CAPTURE_PRIMORDIAL(name, key, kind) \
    m_linkTimeConstants[static_cast<unsigned>(LinkTimeConstant::name)].set(vm, this, \
        JSC_PRIMORDIAL_READ_##kind(this, vm, holder, JSC_PRIMORDIAL_KEY_##kind(key)));

void JSGlobalObject::capturePrimordials(VM& vm, JSObject* holder, PrimordialHolder which)
{
    switch (which) {
#define CAPTURE_PRIMORDIALS_FOR_HOLDER(Holder) \
    case PrimordialHolder::Holder: \
        JSC_FOREACH_PRIMORDIAL_##Holder(CAPTURE_PRIMORDIAL) \
        return;
    JSC_FOREACH_PRIMORDIAL_HOLDER(CAPTURE_PRIMORDIALS_FOR_HOLDER)
#undef CAPTURE_PRIMORDIALS_FOR_HOLDER
    }
    UNUSED_PARAM(holder);
}

#undef CAPTURE_PRIMORDIAL
#undef JSC_PRIMORDIAL_KEY_Method
#undef JSC_PRIMORDIAL_KEY_Getter
#undef JSC_PRIMORDIAL_KEY_SymbolMethod
#undef JSC_PRIMORDIAL_KEY_SymbolGetter
#undef JSC_PRIMORDIAL_READ_Method
#undef JSC_PRIMORDIAL_READ_Getter
#undef JSC_PRIMORDIAL_READ_SymbolMethod
#undef JSC_PRIMORDIAL_READ_SymbolGetter

} // namespace JSC

#endif // USE(BUN_JSC_ADDITIONS)
