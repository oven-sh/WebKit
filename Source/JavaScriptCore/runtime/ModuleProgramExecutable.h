/*
 * Copyright (C) 2009-2022 Apple Inc. All rights reserved.
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

#include "GlobalExecutable.h"

namespace JSC {

class FunctionExecutable;
class SymbolTable;
class UnlinkedModuleProgramCodeBlock;

class ModuleProgramExecutable final : public GlobalExecutable {
    friend class LLIntOffsetsExtractor;
public:
    using Base = GlobalExecutable;
    static constexpr unsigned StructureFlags = Base::StructureFlags | StructureIsImmortal;

    template<typename CellType, SubspaceAccess mode>
    static GCClient::IsoSubspace* subspaceFor(VM& vm)
    {
        return vm.moduleProgramExecutableSpace<mode>();
    }

    // What an imported binding read by this executable's code resolves to: the
    // exporting module's source (whose text fixes the binding's ScopeOffset) and the
    // binding's name there, or for exporters that are not source text modules the
    // ScopeOffset itself.
    struct ImportedBinding {
        RefPtr<UniquedStringImpl> localName;
        RefPtr<SourceProvider> exporterSource;
        RefPtr<UniquedStringImpl> exporterLocalName;
        unsigned offset { 0 };
        bool operator==(const ImportedBinding&) const;
    };
    using ImportedBindings = Vector<ImportedBinding>;

    // moduleScopeSymbolTables: the symbol tables of the lexical environments between
    // the module environment and the global lexical environment (JSModuleLoader::moduleScope);
    // linked code embeds their variables' offsets too.
    static ModuleProgramExecutable* tryCreate(JSGlobalObject*, const SourceCode&, std::optional<ImportedBindings>&&, const Vector<SymbolTable*>& moduleScopeSymbolTables);
    // An executable no record shares (JSModuleRecord::getOrMakeExecutable makes the ones records use).
    JS_EXPORT_PRIVATE static ModuleProgramExecutable* tryCreate(JSGlobalObject*, const SourceCode&);

    static void destroy(JSCell*);

    ModuleProgramCodeBlock* codeBlock() const
    {
        return std::bit_cast<ModuleProgramCodeBlock*>(Base::codeBlock());
    }

    UnlinkedModuleProgramCodeBlock* getUnlinkedCodeBlock(JSGlobalObject*);

    UnlinkedModuleProgramCodeBlock* unlinkedCodeBlock() const
    {
        return std::bit_cast<UnlinkedModuleProgramCodeBlock*>(Base::unlinkedCodeBlock());
    }

    Ref<JSC::JITCode> generatedJITCode()
    {
        return generatedJITCodeForCall();
    }

    inline static Structure* createStructure(VM&, JSGlobalObject*, JSValue);

    DECLARE_INFO;

    DECLARE_VISIT_CHILDREN;

    bool isAsync() const { return features() & AwaitFeature; }

    SymbolTable* moduleEnvironmentSymbolTable() LIFETIME_BOUND { return m_moduleEnvironmentSymbolTable.get(); }

    // Records for one URL and source text whose imports resolve alike share this
    // executable (JSModuleRecord::getOrMakeExecutable), so the function declarations'
    // executables live here rather than per record.
    FunctionExecutable* functionDeclaration(VM&, unsigned index);
    const std::optional<ImportedBindings>& importedBindings() const { return m_importedBindings; }
    bool hasModuleScopeSymbolTables(const Vector<SymbolTable*>&) const;
    // Whether the module environment is created directly in the global lexical environment (JSModuleLoader::moduleScope).
    bool resolvesInGlobalScope() const { return m_moduleScopeSymbolTables.isEmpty(); }

    TemplateObjectMap& ensureTemplateObjectMap(VM&);

private:
    friend class ExecutableBase;
    friend class ScriptExecutable;

    ModuleProgramExecutable(JSGlobalObject*, const SourceCode&, std::optional<ImportedBindings>&&, const Vector<SymbolTable*>& moduleScopeSymbolTables);

    WriteBarrier<SymbolTable> m_moduleEnvironmentSymbolTable;
    FixedVector<WriteBarrier<FunctionExecutable>> m_functionDeclarations;
    std::optional<ImportedBindings> m_importedBindings;
    FixedVector<WriteBarrier<SymbolTable>> m_moduleScopeSymbolTables;
    std::unique_ptr<TemplateObjectMap> m_templateObjectMap;
};

} // namespace JSC
