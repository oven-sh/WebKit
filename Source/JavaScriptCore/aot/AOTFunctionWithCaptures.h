/*
 * Copyright (C) 2026 Anthropic PBC.
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

#pragma once

#if ENABLE(AOT)

#include "JSFunction.h"

namespace JSC {

class JSFunctionWithCaptures final : public JSFunction {
public:
    using Base = JSFunction;
    static constexpr unsigned StructureFlags = Base::StructureFlags;
    static constexpr DestructionMode needsDestruction = DoesNotNeedDestruction;

    template<typename CellType, SubspaceAccess>
    static CompleteSubspace* subspaceFor(VM& vm) { return &vm.heap.cellSpace; }

    static constexpr ptrdiff_t offsetOfCount() { return sizeof(JSFunction); }
    static constexpr ptrdiff_t offsetOfCaptures() { return sizeof(JSFunction) + sizeof(EncodedJSValue); }
    static constexpr size_t allocationSize(unsigned count) { return offsetOfCaptures() + count * sizeof(EncodedJSValue); }

    static JSFunctionWithCaptures* create(VM&, JSScope*, Structure*, uintptr_t executableOrFunctionWord, std::span<const EncodedJSValue>);

    WriteBarrier<Unknown>* captures() { return std::bit_cast<WriteBarrier<Unknown>*>(std::bit_cast<char*>(this) + offsetOfCaptures()); }
    unsigned count() const { return m_count; }

    DECLARE_INFO;
    DECLARE_VISIT_CHILDREN;
    static void analyzeHeap(JSCell*, HeapAnalyzer&);

private:
    JSFunctionWithCaptures(VM& vm, FunctionExecutable* executable, JSScope* scope, Structure* structure, unsigned count)
        : Base(vm, executable, scope, structure)
        , m_count(count)
    {
    }

    uint32_t m_count;
};

} // namespace JSC

#endif // ENABLE(AOT)
