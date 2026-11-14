/*
 * Copyright (C) 2026 Apple Inc. All rights reserved.
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
#include "ModuleLoaderPayload.h"

#include "JSCellInlines.h"
#include "JSPromise.h"

namespace JSC {

const ClassInfo ModuleLoaderPayload::s_info = { "ModuleLoaderPayload"_s, nullptr, nullptr, nullptr, CREATE_METHOD_TABLE(ModuleLoaderPayload) };

#if USE(BUN_JSC_ADDITIONS)
// m_asyncContext is in what was padding of the cell: a payload takes the memory that it took without it.
static_assert(sizeof(ModuleLoaderPayload) <= 48);
#endif

ModuleLoaderPayload::ModuleLoaderPayload(VM& vm, Structure* structure, JSPromise* promise, bool deferred, int64_t referrerAsyncOrder, JSValue asyncContext)
    : Base(vm, structure)
    , m_promise(promise, WriteBarrierEarlyInit)
#if USE(BUN_JSC_ADDITIONS)
    , m_asyncContext(asyncContext, WriteBarrierEarlyInit)
    , m_referrerAsyncOrder(referrerAsyncOrder)
#endif
    , m_deferred(deferred)
{
#if !USE(BUN_JSC_ADDITIONS)
    UNUSED_PARAM(referrerAsyncOrder);
    UNUSED_PARAM(asyncContext);
#endif
}

void ModuleLoaderPayload::finishCreation(VM& vm)
{
    Base::finishCreation(vm);
    ASSERT(inherits(info()));
}

template<typename Visitor>
void ModuleLoaderPayload::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<ModuleLoaderPayload>(cell);
    ASSERT_GC_OBJECT_INHERITS(thisObject, info());
    Base::visitChildren(thisObject, visitor);
    visitor.append(thisObject->m_promise);
    visitor.append(thisObject->m_fulfillment);
#if USE(BUN_JSC_ADDITIONS)
    visitor.append(thisObject->m_asyncContext);
#endif
}

DEFINE_VISIT_CHILDREN(ModuleLoaderPayload);

ModuleLoaderPayload* ModuleLoaderPayload::create(VM& vm, JSPromise* promise, bool deferred, int64_t referrerAsyncOrder, JSValue asyncContext)
{
    ModuleLoaderPayload* instance = new (NotNull, allocateCell<ModuleLoaderPayload>(vm)) ModuleLoaderPayload(vm, vm.moduleLoaderPayloadStructure.get(), promise, deferred, referrerAsyncOrder, asyncContext);
    instance->finishCreation(vm);
    return instance;
}

} // namespace JSC
