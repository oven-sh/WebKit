/*
 * Copyright (C) 2008-2021 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1.  Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 * 2.  Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 * 3.  Neither the name of Apple Inc. ("Apple") nor the names of
 *     its contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE AND ITS CONTRIBUTORS "AS IS" AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL APPLE OR ITS CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
 
#pragma once

#include "CodeBlock.h"
#include "JSGlobalObject.h"
#include "JSSymbolTableObject.h"
#include "SymbolTable.h"

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

class LLIntOffsetsExtractor;

class JSLexicalEnvironment : public JSSymbolTableObject {
    friend class JIT;
    friend class LLIntOffsetsExtractor;
public:
    template<typename CellType, SubspaceAccess>
    static CompleteSubspace* subspaceFor(VM& vm)
    {
        static_assert(CellType::needsDestruction == DoesNotNeedDestruction);
        return &vm.heap.cellSpace;
    }

    using Base = JSSymbolTableObject;
    static constexpr unsigned StructureFlags = Base::StructureFlags | OverridesGetOwnPropertySlot | OverridesGetOwnSpecialPropertyNames | OverridesPut;

    WriteBarrierBase<Unknown>* variables()
    {
        return std::bit_cast<WriteBarrierBase<Unknown>*>(std::bit_cast<char*>(this) + offsetOfVariables());
    }

    bool isValidScopeOffset(ScopeOffset offset)
    {
        return !!offset && offset.offset() < symbolTable()->scopeSize();
    }

    WriteBarrierBase<Unknown>& variableAt(ScopeOffset offset)
    {
        ASSERT(isValidScopeOffset(offset));
        return variables()[offset.offset()];
    }

    static size_t offsetOfVariables()
    {
        return WTF::roundUpToMultipleOf<sizeof(WriteBarrier<Unknown>)>(sizeof(JSLexicalEnvironment));
    }

    static size_t offsetOfVariable(ScopeOffset offset)
    {
        Checked<size_t> scopeOffset = offset.offset();
        return offsetOfVariables() + scopeOffset * sizeof(WriteBarrier<Unknown>);
    }

    static size_t allocationSizeForScopeSize(Checked<size_t> scopeSize)
    {
        return offsetOfVariables() + scopeSize * sizeof(WriteBarrier<Unknown>);
    }

    static size_t allocationSize(SymbolTable* symbolTable)
    {
        return allocationSizeForScopeSize(symbolTable->scopeSize());
    }

    static JSLexicalEnvironment* create(VM&, Structure*, JSScope* currentScope, SymbolTable*, JSValue initialValue);
    static JSLexicalEnvironment* create(VM&, JSGlobalObject*, JSScope* currentScope, SymbolTable*, JSValue initialValue);

#if USE(BUN_JSC_ADDITIONS)
    // A scope that says whose the scripts made under it are, for an embedder to give a module loader as its module
    // scope (JSModuleLoader::moduleScope()), or to make any script under. `whose` is not a cell. The scope is told
    // from any other by its structure (JSGlobalObject::scriptOwnerScopeStructure()), and keeps `whose` in its
    // first variable, which has no name: `symbolTable` is one createScriptOwnerScopeSymbolTable() made, to which
    // the embedder has added the names it wants the scope to have.
    JS_EXPORT_PRIVATE static JSLexicalEnvironment* createScriptOwnerScope(VM&, JSGlobalObject*, JSScope* currentScope, SymbolTable*, JSValue whose);
    JS_EXPORT_PRIVATE static SymbolTable* createScriptOwnerScopeSymbolTable(VM&);
    static constexpr ScopeOffset whoseOffset() { return ScopeOffset(0); }
    // Whose the scripts made under a script owner scope are.
    JSValue whose() { return variableAt(whoseOffset()).get(); }
    // The script owner scope `scope` is under, or is, if any. `hops`: how many scopes it is from `scope`.
    JS_EXPORT_PRIVATE static JSLexicalEnvironment* scriptOwnerScopeOf(JSScope*, unsigned& hops);
#endif

    static bool getOwnPropertySlot(JSObject*, JSGlobalObject*, PropertyName, PropertySlot&);
    static void getOwnSpecialPropertyNames(JSObject*, JSGlobalObject*, PropertyNameArrayBuilder&, DontEnumPropertiesMode);

    static bool put(JSCell*, JSGlobalObject*, PropertyName, JSValue, PutPropertySlot&);

    static bool deleteProperty(JSCell*, JSGlobalObject*, PropertyName, DeletePropertySlot&);

    DECLARE_INFO;

    DECLARE_VISIT_CHILDREN;

    inline static Structure* createStructure(VM&, JSGlobalObject*);

protected:
    JSLexicalEnvironment(VM&, Structure*, JSScope*, SymbolTable*, JSValue initialValue);

    DECLARE_DEFAULT_FINISH_CREATION;

    static void analyzeHeap(JSCell*, HeapAnalyzer&);
};

} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
