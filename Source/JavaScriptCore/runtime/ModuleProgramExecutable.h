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
class UnlinkedFunctionExecutable;
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
    JS_EXPORT_PRIVATE static ModuleProgramExecutable* tryCreate(JSGlobalObject*, const SourceCode&, std::optional<ImportedBindings>&& = std::nullopt, const Vector<SymbolTable*>& moduleScopeSymbolTables = { });

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

    // One per executable, from its first unlinked code on: every environment of every record that shares the executable
    // is made from it (see getUnlinkedCodeBlock).
    SymbolTable* moduleEnvironmentSymbolTable() LIFETIME_BOUND { return m_moduleEnvironmentSymbolTable.get(); }
    // The mode of the code that table came from, and of any code fetched for this executable later.
    OptionSet<CodeGenerationMode> codeGenerationMode() const { return m_codeGenerationMode; }

    // Records for one URL and source text whose imports resolve alike share this
    // executable (JSModuleRecord::getOrMakeExecutable), so the function declarations'
    // executables live here rather than per record.
    FunctionExecutable* functionDeclaration(VM&, unsigned index);
    // The same for a caller that has the declaration's unlinked executable from elsewhere (a module record that instantiates
    // a declaration when its binding is first read may do so after this executable let go of its unlinked code):
    // linkedFunctionDeclaration() first, and linkFunctionDeclaration() with the module's own functionDecl(index) if that is null.
    FunctionExecutable* linkedFunctionDeclaration(unsigned index) const { return index < m_functionDeclarations.size() ? m_functionDeclarations[index].get() : nullptr; }
    FunctionExecutable* linkFunctionDeclaration(VM&, unsigned index, UnlinkedFunctionExecutable*);
    const std::optional<ImportedBindings>& importedBindings() const { return m_importedBindings; }
    bool hasModuleScopeSymbolTables(const Vector<SymbolTable*>&) const;
    // Whether the module environment is created directly in the global lexical environment (JSModuleLoader::moduleScope).
    bool resolvesInGlobalScope() const { return m_moduleScopeSymbolTables.isEmpty(); }

    TemplateObjectMap& ensureTemplateObjectMap(VM&);

    // Options::useSharedModuleFunctionExpressionExecutables(): the executable of function expression `index` of the
    // module's top-level code, linked from `unlinkedExecutable` the first time.
    FunctionExecutable* functionExpression(VM&, unsigned index, unsigned numberOfFunctionExpressions, UnlinkedFunctionExecutable*);

    // Every record that is going to evaluate the module with this executable says so (JSModuleRecord::getOrMakeExecutable),
    // and says when its body ran to completion. Once none is left, and until another record adopts the executable, nothing
    // needs the linked code; the unlinked code is dropped too if it can be had back for the asking.
    void willBeEvaluatedByAnotherRecord() { ++m_recordsYetToFinishEvaluation; }
    void didFinishEvaluation(VM&);
    bool hasFinishedEvaluation() const { return m_hasBeenEvaluated && !m_recordsYetToFinishEvaluation; }
    // releaseUnlinkedCodeIfRecoverable() took the unlinked code; getUnlinkedCodeBlock() decodes the same code again.
    bool hasReleasedUnlinkedCode() const { return m_hasReleasedUnlinkedCode; }
    // Only once the body has finished (an environment made for code that has yet to run is tied to that very code, see
    // UnlinkedModuleProgramCodeBlock.h), and only if getUnlinkedCodeBlock() can decode the same code again.
    void releaseUnlinkedCodeIfRecoverable(VM&);

private:
    friend class ExecutableBase;
    friend class ScriptExecutable;

    ModuleProgramExecutable(JSGlobalObject*, const SourceCode&, std::optional<ImportedBindings>&&, const Vector<SymbolTable*>& moduleScopeSymbolTables);

    WriteBarrier<SymbolTable> m_moduleEnvironmentSymbolTable;
    FixedVector<WriteBarrier<FunctionExecutable>> m_functionDeclarations;
    std::optional<ImportedBindings> m_importedBindings;
    FixedVector<WriteBarrier<SymbolTable>> m_moduleScopeSymbolTables;
    FixedVector<WriteBarrier<FunctionExecutable>> m_functionExpressions;
    unsigned m_recordsYetToFinishEvaluation { 0 };
    bool m_hasBeenEvaluated { false };
    bool m_hasReleasedUnlinkedCode { false };
    OptionSet<CodeGenerationMode> m_codeGenerationMode;
    std::unique_ptr<TemplateObjectMap> m_templateObjectMap;
};

} // namespace JSC
