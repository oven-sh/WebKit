/*
 * Copyright (C) 2016 Yusuke Suzuki <utatane.tea@gmail.com>
 * Copyright (C) 2019-2022 Apple Inc. All rights reserved.
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

#include "JSObject.h"
#include "SourceCode.h"

namespace JSC {

class JSSourceCode final : public JSCell {
public:
    using Base = JSCell;

    static constexpr unsigned StructureFlags = Base::StructureFlags | StructureIsImmortal;
    static constexpr DestructionMode needsDestruction = NeedsDestruction;

    DECLARE_EXPORT_INFO;
#if USE(BUN_JSC_ADDITIONS)
    DECLARE_VISIT_CHILDREN;
#endif

    template<typename CellType, SubspaceAccess mode>
    static GCClient::IsoSubspace* subspaceFor(VM& vm)
    {
        return vm.sourceCodeSpace<mode>();
    }

    inline static Structure* createStructure(VM&, JSGlobalObject*, JSValue);

    static JSSourceCode* create(VM& vm, Structure* structure, SourceCode&& sourceCode)
    {
        auto* result = new (NotNull, allocateCell<JSSourceCode>(vm)) JSSourceCode(vm, structure, WTF::move(sourceCode));
        result->finishCreation(vm);
        return result;
    }

    static JSSourceCode* create(VM& vm, SourceCode&& sourceCode)
    {
        return create(vm, vm.sourceCodeStructure.get(), WTF::move(sourceCode));
    }

#if USE(BUN_JSC_ADDITIONS)
    // The source of a synthetic module that exports `payload`. JSModuleLoader::makeModule() passes `payload` to
    // `generator` each time it makes a module from this source, and this cell keeps `payload` alive until then.
    JS_EXPORT_PRIVATE static JSSourceCode* createWithPayload(VM&, SyntheticSourceProvider::PayloadSyntheticSourceGenerator, JSValue payload, const SourceOrigin&, String&& sourceURL);

    JSValue payload() const { return m_payload.get(); }
#endif

    const SourceCode& sourceCode() const
    {
        return m_sourceCode;
    }

    static void destroy(JSCell*);

private:
    JSSourceCode(VM& vm, Structure* structure, SourceCode&& sourceCode)
        : Base(vm, structure)
        , m_sourceCode(WTF::move(sourceCode))
    {
    }

    SourceCode m_sourceCode;
#if USE(BUN_JSC_ADDITIONS)
    WriteBarrier<Unknown> m_payload;
#endif
};

#if USE(BUN_JSC_ADDITIONS) && CPU(ADDRESS64)
// The payload takes the bytes that rounding up to the cell size left unused: a JSSourceCode is a 32-byte cell with it
// and without it.
static_assert(sizeof(JSSourceCode) <= 32);
#endif

} // namespace JSC
