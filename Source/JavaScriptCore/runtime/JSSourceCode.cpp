/*
 * Copyright (C) 2016 Yusuke Suzuki <utatane.tea@gmail.com>
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
#include "JSSourceCode.h"

#include "JSCellInlines.h"

namespace JSC {

const ClassInfo JSSourceCode::s_info = { "JSSourceCode"_s, nullptr, nullptr, nullptr, CREATE_METHOD_TABLE(JSSourceCode) };

void JSSourceCode::destroy(JSCell* cell)
{
    static_cast<JSSourceCode*>(cell)->~JSSourceCode();
}

#if USE(BUN_JSC_ADDITIONS)
JSSourceCode* JSSourceCode::createWithPayload(VM& vm, SyntheticSourceProvider::PayloadSyntheticSourceGenerator generator, JSValue payload, const SourceOrigin& sourceOrigin, String&& sourceURL)
{
    ASSERT(payload);
    auto* result = create(vm, SourceCode(SyntheticSourceProvider::createWithPayloadGenerator(generator, sourceOrigin, WTF::move(sourceURL))));
    result->m_payload.set(vm, result, payload);
    return result;
}

template<typename Visitor>
void JSSourceCode::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<JSSourceCode>(cell);
    ASSERT_GC_OBJECT_INHERITS(thisObject, info());
    Base::visitChildren(thisObject, visitor);
    visitor.append(thisObject->m_payload);
}

DEFINE_VISIT_CHILDREN(JSSourceCode);
#endif

} // namespace JSC
