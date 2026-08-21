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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "JSAsyncFunctionGenerator.h"

#include "JSCInlines.h"
#include "InternalFieldTuple.h"
#include "JSGlobalObject.h"
#include "JSInternalFieldObjectImplInlines.h"

namespace JSC {

const ClassInfo JSAsyncFunctionGenerator::s_info = { "AsyncFunctionGenerator"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(JSAsyncFunctionGenerator) };

#if USE(BUN_JSC_ADDITIONS)
// The AsyncContext slot starts out holding the async context that was current
// when the async function was called, so asyncFunctionDrive can tell whether
// the function's synchronous prefix changed it (see JSMicrotask.cpp). The
// DFG/FTL inline allocation paths do the same load.
static ALWAYS_INLINE void recordEntryAsyncContext(VM& vm, JSAsyncFunctionGenerator* generator, Structure* structure)
{
    if (auto* data = structure->globalObject()->m_asyncContextData.get()) {
        JSValue current = data->getInternalField(0);
        if (!current.isUndefined())
            generator->setAsyncContext(vm, current);
    }
}
#endif

JSAsyncFunctionGenerator* JSAsyncFunctionGenerator::create(VM& vm, Structure* structure)
{
    JSAsyncFunctionGenerator* generator = new (NotNull, allocateCell<JSAsyncFunctionGenerator>(vm)) JSAsyncFunctionGenerator(vm, structure);
    generator->finishCreation(vm);
#if USE(BUN_JSC_ADDITIONS)
    recordEntryAsyncContext(vm, generator, structure);
#endif
    return generator;
}

JSAsyncFunctionGenerator* JSAsyncFunctionGenerator::createWithInitialValues(VM& vm, Structure* structure)
{
    JSAsyncFunctionGenerator* generator = new (NotNull, allocateCell<JSAsyncFunctionGenerator>(vm)) JSAsyncFunctionGenerator(vm, structure);
    generator->finishCreation(vm);
#if USE(BUN_JSC_ADDITIONS)
    recordEntryAsyncContext(vm, generator, structure);
#endif
    return generator;
}

Structure* JSAsyncFunctionGenerator::createStructure(VM& vm, JSGlobalObject* globalObject, JSValue prototype)
{
    return Structure::create(vm, globalObject, prototype, TypeInfo(JSAsyncFunctionGeneratorType, StructureFlags), info());
}

JSAsyncFunctionGenerator::JSAsyncFunctionGenerator(VM& vm, Structure* structure)
    : Base(vm, structure)
{
}

void JSAsyncFunctionGenerator::finishCreation(VM& vm)
{
    Base::finishCreation(vm);
    auto values = initialValues();
    ASSERT(values.size() == numberOfInternalFields);
    for (unsigned index = 0; index < values.size(); ++index)
        internalField(index).set(vm, this, values[index]);
}

template<typename Visitor>
void JSAsyncFunctionGenerator::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<JSAsyncFunctionGenerator>(cell);
    ASSERT_GC_OBJECT_INHERITS(thisObject, info());
    Base::visitChildren(thisObject, visitor);
}

DEFINE_VISIT_CHILDREN(JSAsyncFunctionGenerator);

} // namespace JSC
