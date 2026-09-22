/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
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

#if USE(BUN_JSC_ADDITIONS)

#include "JSLexicalEnvironment.h"

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

// A scope that script belongs to. A function of script made under one runs with it as the current script execution
// owner (the second field of JSGlobalObject::m_asyncContextData) whoever calls it: op_enter compares the two
// (CodeBlock::scriptExecutionOwnerDepth()). An embedder makes one as the module scope of a module loader of its own
// (JSModuleLoader::moduleScope()) and keeps in it the object of its own that the scope stands for.
class JSScriptExecutionOwnerEnvironment final : public JSLexicalEnvironment {
public:
    using Base = JSLexicalEnvironment;
    static constexpr unsigned StructureFlags = Base::StructureFlags;

    JS_EXPORT_PRIVATE static JSScriptExecutionOwnerEnvironment* create(VM&, JSGlobalObject*, JSScope* currentScope, SymbolTable*, JSValue initialValue);

    DECLARE_EXPORT_INFO;

    DECLARE_VISIT_CHILDREN;

    inline static Structure* createStructure(VM&, JSGlobalObject*);

    JSCell* embedderObject() { return embedderObjectSlot().get(); }
    void setEmbedderObject(VM& vm, JSCell* object) { embedderObjectSlot().set(vm, this, object); }

private:
    JSScriptExecutionOwnerEnvironment(VM&, Structure*, JSScope*, SymbolTable*, JSValue initialValue);

    DECLARE_DEFAULT_FINISH_CREATION;

    // After the variable slots, as JSModuleEnvironment's module record is: a member would overlap them.
    static size_t offsetOfEmbedderObject(SymbolTable* symbolTable)
    {
        size_t offset = Base::allocationSize(symbolTable);
        ASSERT(WTF::roundUpToMultipleOf<sizeof(WriteBarrier<JSCell>)>(offset) == offset);
        return offset;
    }

    static size_t allocationSize(SymbolTable* symbolTable)
    {
        return offsetOfEmbedderObject(symbolTable) + sizeof(WriteBarrier<JSCell>);
    }

    WriteBarrierBase<JSCell>& embedderObjectSlot()
    {
        return *std::bit_cast<WriteBarrierBase<JSCell>*>(std::bit_cast<char*>(this) + offsetOfEmbedderObject(symbolTable()));
    }
};

} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // USE(BUN_JSC_ADDITIONS)
