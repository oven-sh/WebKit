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

#include "JSFunction.h"
#include "ScriptExecutable.h"
#include "SourceCode.h"
#include <wtf/Box.h>
#include <wtf/Markable.h>

namespace JSC {

struct FunctionOverrideInfo;

class FunctionExecutable final : public ScriptExecutable {
    friend class JIT;
    friend class LLIntOffsetsExtractor;
public:
    typedef ScriptExecutable Base;
    static constexpr unsigned StructureFlags = Base::StructureFlags | StructureIsImmortal;

    template<typename CellType, SubspaceAccess>
    static GCClient::IsoSubspace* subspaceFor(VM& vm)
    {
        return &vm.functionExecutableSpace();
    }

    static FunctionExecutable* create(VM& vm, ScriptExecutable* topLevelExecutable, const SourceCode& source, UnlinkedFunctionExecutable* unlinkedExecutable, Intrinsic intrinsic, bool isInsideOrdinaryFunction)
    {
        FunctionExecutable* executable = new (NotNull, allocateCell<FunctionExecutable>(vm)) FunctionExecutable(vm, topLevelExecutable, source, unlinkedExecutable, intrinsic, isInsideOrdinaryFunction);
        executable->finishCreation(vm);
        return executable;
    }
    static FunctionExecutable* fromGlobalCode(const Identifier& name, JSGlobalObject*, String&& program, const SourceOrigin&, SourceTaintedOrigin, const String& sourceURL, const TextPosition&, LexicallyScopedFeatures, JSObject*& exception, int overrideLineNumber, std::optional<int> functionConstructorParametersEndPosition, FunctionConstructionMode);

    static void destroy(JSCell*);
        
    // The short form. A function that was compiled when the program was built, and has no other code and never will, has no use for
    // most of this: there is nothing to parse, nothing to compile, nothing to watch, and nothing that changes. Its executable
    // (StaticHeap makes them) ends where ScriptExecutable's m_source would be, and says so by its type: what calls a function goes by
    // what comes before that, and whatever tells a FunctionExecutable by FunctionExecutableType, to get at its CodeBlock, finds none.
    // The rest of what there is to say about it is in a table (StaticHeap::rowOf()), where whoever asks here looks:
    // its name, how many parameters it has, which module it is in, and an UnlinkedFunctionExecutable that says everything else, which it
    // shares with every function of which that is the same.
    static constexpr size_t sizeOfShortForm = 64;
    inline static Structure* createStructureOfShortForm(VM&, JSGlobalObject*, JSValue);

    const FunctionExecutable* inFull() const { return WTF::opaque(this); }
    FunctionExecutable* inFull() { return WTF::opaque(this); }

    UnlinkedFunctionExecutable* unlinkedExecutable() const
    {
        if (isShortForm()) [[unlikely]]
            return StaticHeap::unlinkedFunctionOf(StaticHeap::rowOf(indexOfShortForm()));
        return inFull()->m_unlinkedExecutable.get();
    }

    // Returns either call or construct bytecode. This can be appropriate
    // for answering questions that that don't vary between call and construct --
    // for example, argumentsRegister().
    FunctionCodeBlock* eitherCodeBlock() const
    {
        if (auto* result = codeBlockForCall())
            return result;
        return codeBlockForConstruct();
    }
        
    bool isGeneratedForCall() const
    {
        return !!codeBlockForCall();
    }

    FunctionCodeBlock* codeBlockForCall() const
    {
        return isShortForm() ? nullptr : std::bit_cast<FunctionCodeBlock*>(inFull()->m_codeBlockForCall.get());
    }

    bool isGeneratedForConstruct() const
    {
        return !!codeBlockForConstruct();
    }

    FunctionCodeBlock* codeBlockForConstruct() const
    {
        return isShortForm() ? nullptr : std::bit_cast<FunctionCodeBlock*>(inFull()->m_codeBlockForConstruct.get());
    }
        
    bool isGeneratedFor(CodeSpecializationKind kind)
    {
        if (kind == CodeSpecializationKind::CodeForCall)
            return isGeneratedForCall();
        ASSERT(kind == CodeSpecializationKind::CodeForConstruct);
        return isGeneratedForConstruct();
    }
        
    FunctionCodeBlock* codeBlockFor(CodeSpecializationKind kind)
    {
        if (kind == CodeSpecializationKind::CodeForCall)
            return codeBlockForCall();
        ASSERT(kind == CodeSpecializationKind::CodeForConstruct);
        return codeBlockForConstruct();
    }

    FunctionCodeBlock* baselineCodeBlockFor(CodeSpecializationKind);
        
    FunctionCodeBlock* profiledCodeBlockFor(CodeSpecializationKind kind)
    {
        return baselineCodeBlockFor(kind);
    }

    FunctionCodeBlock* replaceCodeBlockWith(VM&, CodeSpecializationKind, CodeBlock*);

    RefPtr<TypeSet> returnStatementTypeSet() 
    {
        RareData& rareData = ensureRareData();
        if (!rareData.m_returnStatementTypeSet)
            rareData.m_returnStatementTypeSet = TypeSet::create();
        return rareData.m_returnStatementTypeSet;
    }
        
    FunctionMode functionMode() { return unlinkedExecutable()->functionMode(); }
    ImplementationVisibility implementationVisibility() const { return unlinkedExecutable()->implementationVisibility(); }
    bool isBuiltinFunction() const { return unlinkedExecutable()->isBuiltinFunction(); }
    bool isPrivateBuiltinFunction() const { return isBuiltinFunction() && (!sourceProvider() || !sourceProvider()->sourceURL()); }
    ConstructAbility constructAbility() const { return unlinkedExecutable()->constructAbility(); }
    InlineAttribute inlineAttribute() const { return unlinkedExecutable()->inlineAttribute(); }
    bool isClass() const { return unlinkedExecutable()->isClass(); }
    bool isArrowFunction() const { return parseMode() == SourceParseMode::ArrowFunctionMode; }
    bool isGetter() const { return parseMode() == SourceParseMode::GetterMode; }
    bool isSetter() const { return parseMode() == SourceParseMode::SetterMode; }
    bool isGenerator() const { return isGeneratorParseMode(parseMode()); }
    bool isAsyncGenerator() const { return isAsyncGeneratorParseMode(parseMode()); }
    bool isMethod() const { return parseMode() == SourceParseMode::MethodMode; }
    bool hasPrototypeProperty() const
    {
        return SourceParseModeSet(
            SourceParseMode::NormalFunctionMode,
            SourceParseMode::GeneratorBodyMode,
            SourceParseMode::GeneratorWrapperFunctionMode,
            SourceParseMode::GeneratorWrapperMethodMode,
            SourceParseMode::AsyncGeneratorWrapperFunctionMode,
            SourceParseMode::AsyncGeneratorWrapperMethodMode,
            SourceParseMode::AsyncGeneratorBodyMode
        ).contains(parseMode()) || isClass();
    }
    DerivedContextType derivedContextType() const { return unlinkedExecutable()->derivedContextType(); }
    bool isClassConstructorFunction() const { return unlinkedExecutable()->isClassConstructorFunction(); }
    const Identifier& name()
    {
        if (isShortForm()) [[unlikely]]
            return unlinkedExecutable()->hasName() ? ecmaName() : unlinkedExecutable()->name();
        return inFull()->m_unlinkedExecutable->name();
    }
    const Identifier& ecmaName()
    {
        if (isShortForm()) [[unlikely]]
            return StaticHeap::rowOf(indexOfShortForm()).name();
        return inFull()->m_unlinkedExecutable->ecmaName();
    }
    // Unlike name() / ecmaName(), also callable from the collector's end phase (ErrorInstance::computeErrorInfo's stack traces).
    String nameWithoutGC() { return isShortForm() ? (unlinkedExecutable()->hasName() ? ecmaNameWithoutGC() : String()) : inFull()->m_unlinkedExecutable->nameWithoutGC(); }
    String ecmaNameWithoutGC() { return isShortForm() ? ecmaName().string() : inFull()->m_unlinkedExecutable->ecmaNameWithoutGC(); }
    const Identifier* tryGetEcmaNameConcurrently() { return isShortForm() ? &ecmaName() : inFull()->m_unlinkedExecutable->tryGetEcmaNameConcurrently(); } // null while the name is still only in the bytecode cache; otherwise &ecmaName() (which may itself be a null Identifier)
    CString inferredNameForTools(); // dumps and debug info; callable from compiler / GC threads, where a name still in the bytecode cache prints as a placeholder
    unsigned parameterCount() const { return isShortForm() ? StaticHeap::rowOf(indexOfShortForm()).parameterCount : inFull()->m_unlinkedExecutable->parameterCount(); } // Excluding 'this'!
    SourceParseMode parseMode() const { return unlinkedExecutable()->parseMode(); }
    JSParserScriptMode scriptMode() const { return unlinkedExecutable()->scriptMode(); }
    SourceCode classSource() const
    {
        // (The source of a constructor that nobody wrote is one of the engine's own; the class is in the module's.)
        if (isShortForm()) [[unlikely]]
            return unlinkedExecutable()->classSource(*sourceProvider());
        bool isInTopLevelSource = inFull()->m_unlinkedExecutable->isBuiltinDefaultClassConstructor() && inFull()->m_topLevelExecutable;
        return inFull()->m_unlinkedExecutable->classSource(*(isInTopLevelSource ? inFull()->m_topLevelExecutable->source() : source()).provider());
    }

    DECLARE_VISIT_CHILDREN;
    DECLARE_VISIT_OUTPUT_CONSTRAINTS;
    inline static Structure* createStructure(VM&, JSGlobalObject*, JSValue);

    static constexpr int overrideLineNumberNotFound = -1;
    void setOverrideLineNumber(int overrideLineNumber)
    {
        if (overrideLineNumber == overrideLineNumberNotFound) {
            if (auto* rareData = this->rareData()) [[unlikely]]
                rareData->m_overrideLineNumber = std::nullopt;
            return;
        }
        ensureRareData().m_overrideLineNumber = overrideLineNumber;
    }

    std::optional<int> overrideLineNumber() const
    {
        if (auto* rareData = this->rareData()) [[unlikely]]
            return rareData->m_overrideLineNumber;
        return std::nullopt;
    }

    int lineCount() const
    {
        if (auto* rareData = this->rareData()) [[unlikely]]
            return rareData->m_lineCount;
        if (isShortForm()) [[unlikely]]
            return 0; // (Where it ends, and where it is in a text that is not there, is nothing that anybody has a use for.)
        return inFull()->m_unlinkedExecutable->lineCount();
    }

    int endColumn() const
    {
        if (auto* rareData = this->rareData()) [[unlikely]]
            return rareData->m_endColumn;
        if (isShortForm()) [[unlikely]]
            return startColumn();
        return inFull()->m_unlinkedExecutable->linkedEndColumn(inFull()->m_source.startColumn().oneBasedInt());
    }

    int firstLine() const
    {
        return source().firstLine().oneBasedInt();
    }

    int lastLine() const
    {
        return firstLine() + lineCount();
    }

    unsigned functionEnd() const
    {
        if (auto* rareData = this->rareData()) [[unlikely]]
            return rareData->m_functionEnd;
        if (isShortForm()) [[unlikely]]
            return 0;
        return inFull()->m_unlinkedExecutable->unlinkedFunctionEnd();
    }

    unsigned functionStart() const
    {
        if (auto* rareData = this->rareData()) [[unlikely]]
            return rareData->m_functionStart;
        if (isShortForm()) [[unlikely]]
            return 0;
        return inFull()->m_unlinkedExecutable->unlinkedFunctionStart();
    }

    unsigned parametersStartOffset() const
    {
        if (auto* rareData = this->rareData()) [[unlikely]]
            return rareData->m_parametersStartOffset;
        if (isShortForm()) [[unlikely]]
            return 0;
        return inFull()->m_unlinkedExecutable->parametersStartOffset();
    }

    void overrideInfo(const FunctionOverrideInfo&);

    DECLARE_EXPORT_INFO;

    bool singletonHasBeenInvalidated() const { return isShortForm() || inFull()->m_singleton.hasBeenInvalidated(); }
    InferredValue<JSFunction>& singleton()
    {
        RELEASE_ASSERT(!isShortForm());
        return inFull()->m_singleton;
    }

    void notifyCreation(VM&, JSFunction*, const char* reason);

    // Cached poly proto structure for the result of constructing this executable.
    Structure* cachedPolyProtoStructure()
    {
        if (auto* rareData = this->rareData()) [[unlikely]]
            return rareData->m_cachedPolyProtoStructureID.get();
        return nullptr;
    }
    void setCachedPolyProtoStructure(VM& vm, Structure* structure)
    {
        ensureRareData().m_cachedPolyProtoStructureID.set(vm, this, structure);
    }

    InlineWatchpointSet& ensurePolyProtoWatchpoint()
    {
        RareData& rareData = ensureRareData();
        if (!rareData.m_polyProtoWatchpoint)
            rareData.m_polyProtoWatchpoint = Box<InlineWatchpointSet>::create(IsWatched);
        return *rareData.m_polyProtoWatchpoint;
    }

    Box<InlineWatchpointSet> sharedPolyProtoWatchpoint() const { return rareData() ? rareData()->m_polyProtoWatchpoint : nullptr; }

    ScriptExecutable* topLevelExecutable() const LIFETIME_BOUND
    {
        if (isShortForm())
            return topLevelExecutableOfStaticExecutable();
        if (ScriptExecutable* result = inFull()->m_topLevelExecutable.get()) [[likely]]
            return result;
        return topLevelExecutableOfStaticExecutable();
    }

    // Of one that was made when the program was built, and is nobody's to write to: where the code that was compiled for it then is,
    // and which function that is (AOT::CodeHeader::index). Its entry points are AOT::Stub::EnterStaticFunctionFor*, which go by these.
    void* aotEntryFor(CodeSpecializationKind kind) const { return m_aotEntry[static_cast<unsigned>(kind)]; }
    uint32_t aotIndexFor(CodeSpecializationKind kind) const { return m_aotIndex[static_cast<unsigned>(kind)]; }
    void setUnlinkedExecutableWhileStaticHeapIsBuilt(UnlinkedFunctionExecutable* unlinked) { inFull()->m_unlinkedExecutable.setWithoutWriteBarrier(unlinked); }
    JS_EXPORT_PRIVATE void becomeStatic(VM&);
    JS_EXPORT_PRIVATE void setAOTCode(CodeSpecializationKind, void* stub, void* entry, uint32_t index);
    // There is code to call it with and none to construct with, and to construct is to make an object, call it, and see what
    // comes back (AOT::Stub::ConstructByCalling, which is then what aotEntryFor() is). That will do for a function that has no way
    // of telling: see generateUnlinkedCodeBlockForFunctions().
    static constexpr uint32_t aotIndexOfWhatConstructsByCalling = std::numeric_limits<uint32_t>::max();
    bool constructsByCalling() const { return m_aotIndex[static_cast<unsigned>(CodeSpecializationKind::CodeForConstruct)] == aotIndexOfWhatConstructsByCalling; }
    static constexpr ptrdiff_t offsetOfAOTEntryFor(CodeSpecializationKind kind) { return OBJECT_OFFSETOF(FunctionExecutable, m_aotEntry) + static_cast<unsigned>(kind) * sizeof(void*); }
    static constexpr ptrdiff_t offsetOfAOTIndexFor(CodeSpecializationKind kind) { return OBJECT_OFFSETOF(FunctionExecutable, m_aotIndex) + static_cast<unsigned>(kind) * sizeof(uint32_t); }

    TemplateObjectMap& ensureTemplateObjectMap(VM&);

    void reconcileWeakReferencesAtGCEnd(VM&, CollectionScope);

    JSString* toString(JSGlobalObject*);
    JSString* asStringConcurrently() const
    {
        if (!rareData())
            return nullptr;
        return rareData()->m_asString.get();
    }

    static constexpr ptrdiff_t offsetOfRareData() { return OBJECT_OFFSETOF(FunctionExecutable, m_rareData); }
    static constexpr ptrdiff_t offsetOfCodeBlockForCall() { return OBJECT_OFFSETOF(FunctionExecutable, m_codeBlockForCall); }
    static constexpr ptrdiff_t offsetOfCodeBlockForConstruct() { return OBJECT_OFFSETOF(FunctionExecutable, m_codeBlockForConstruct); }

    static constexpr ptrdiff_t offsetOfCodeBlockFor(CodeSpecializationKind kind)
    {
        switch (kind) {
        case CodeSpecializationKind::CodeForCall:
            return OBJECT_OFFSETOF(FunctionExecutable, m_codeBlockForCall);
        case CodeSpecializationKind::CodeForConstruct:
            return OBJECT_OFFSETOF(FunctionExecutable, m_codeBlockForConstruct);
        }
        RELEASE_ASSERT_NOT_REACHED();
        return 0;
    }

    struct RareData {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(RareData);

        static constexpr ptrdiff_t offsetOfAsString() { return OBJECT_OFFSETOF(RareData, m_asString); }

        RefPtr<TypeSet> m_returnStatementTypeSet;
        unsigned m_lineCount;
        unsigned m_endColumn;
        Markable<int> m_overrideLineNumber;
        unsigned m_parametersStartOffset { 0 };
        WriteBarrierStructureID m_cachedPolyProtoStructureID;
        std::unique_ptr<TemplateObjectMap> m_templateObjectMap;
        WriteBarrier<JSString> m_asString;
        unsigned m_functionStart { UINT_MAX };
        unsigned m_functionEnd { UINT_MAX };
        Box<InlineWatchpointSet> m_polyProtoWatchpoint;
    };

private:
    friend class ExecutableBase;
    FunctionExecutable(VM&, ScriptExecutable* topLevelExecutable, const SourceCode&, UnlinkedFunctionExecutable*, Intrinsic, bool isInsideOrdinaryFunction);

    DECLARE_DEFAULT_FINISH_CREATION;

    friend class ScriptExecutable;

    RareData* rareData() const { return isShortForm() ? nullptr : inFull()->m_rareData.get(); }
    RareData& ensureRareData()
    {
        RELEASE_ASSERT(!isShortForm());
        if (inFull()->m_rareData) [[likely]]
            return *inFull()->m_rareData;
        return ensureRareDataSlow();
    }
    RareData& ensureRareDataSlow();

    JSString* toStringSlow(JSGlobalObject*);

    // FIXME: We can merge rareData pointer and top-level executable pointer. First time, setting parent.
    // If RareData is required, materialize RareData, swap it, and store top-level executable pointer inside RareData.
    // https://bugs.webkit.org/show_bug.cgi?id=197625
    std::unique_ptr<RareData> m_rareData;
    WriteBarrier<ScriptExecutable> m_topLevelExecutable;
    WriteBarrier<UnlinkedFunctionExecutable> m_unlinkedExecutable;
    WriteBarrier<CodeBlock> m_codeBlockForCall;
    WriteBarrier<CodeBlock> m_codeBlockForConstruct;
    InferredValue<JSFunction> m_singleton;

    JS_EXPORT_PRIVATE ScriptExecutable* topLevelExecutableOfStaticExecutable() const;
};

} // namespace JSC
