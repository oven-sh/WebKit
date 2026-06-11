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
        
    UnlinkedFunctionExecutable* unlinkedExecutable() const
    {
        return m_unlinkedExecutable.get();
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

    // TSAN code-lifecycle (TSAN-TRIAGE.md section 3.5): m_codeBlockForCall /
    // m_codeBlockForConstruct are published by a concurrent installCode
    // (replaceCodeBlockWith) and retracted by clearCode while read lock-free
    // here (prepareForExecution fast gate, CodeBlock::replacement). Read the
    // WriteBarrier slot with a consume-ordered atomic load (relaxed in
    // production — identical codegen to the plain load; acquire under TSAN
    // only, see jit/JITCode.h). The writer side lives in
    // ScriptExecutable.cpp: installCode publishes with an atomic release
    // store and clearCode retracts with one (both via the slot, preserving
    // the GC write barrier); the legacy replaceCodeBlockWith plain-store
    // path is no longer called — see the note at its declaration below.
    FunctionCodeBlock* codeBlockForCall() const
    {
        CodeBlock* codeBlock = WTF::atomicLoad(const_cast<WriteBarrier<CodeBlock>&>(m_codeBlockForCall).slot(), JITCodePointerConsumeOrder);
        return std::bit_cast<FunctionCodeBlock*>(codeBlock);
    }

    bool isGeneratedForConstruct() const
    {
        return !!codeBlockForConstruct();
    }

    FunctionCodeBlock* codeBlockForConstruct() const
    {
        // See codeBlockForCall above.
        CodeBlock* codeBlock = WTF::atomicLoad(const_cast<WriteBarrier<CodeBlock>&>(m_codeBlockForConstruct).slot(), JITCodePointerConsumeOrder);
        return std::bit_cast<FunctionCodeBlock*>(codeBlock);
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

    // UNGIL FIX-2 (publish/observe pairing — ANNEX CBI item 3, generalized):
    // gilOff, dispatch publication must derive the entrypoint from the SAME
    // CodeBlock object it transfers into CallFrameSlot::codeBlock. Pairing a
    // separately (re-)read m_jitCodeForCall/m_jitCodeForConstruct mirror with
    // codeBlockFor(kind) tears across a concurrent installCode (IT-8 store
    // sequence: retract gate -> fence -> publish CodeBlock -> fence -> publish
    // mirror) and enters one tier's machine code with another tier's CodeBlock
    // in the frame slot — AHInvalidCodeBlock at the callee-prologue asserts
    // with useJITAsserts, wrong JITData / null argument-profile storage in
    // release.
    //
    // Returns the snapshot CodeBlock (nullptr if none is installed).
    // entrypointOut is null iff no dispatchable code exists, in which case the
    // caller must take its slow path. jitCodeKeeperOut receives the owning
    // JITCode ref for the returned entrypoint: the CALLER must hold it until
    // the call/install that consumes entrypointOut has completed — the
    // entrypoint is a raw CodePtr with no other keeper, and a concurrent
    // jettison + sweep may otherwise free the machine code between return and
    // use. Defined in bytecode/CodeBlock.cpp.
    FunctionCodeBlock* codeBlockWithEntrypointFor(CodeSpecializationKind, ArityCheckMode, CodePtr<JSEntryPtrTag>& entrypointOut, RefPtr<JSC::JITCode>& jitCodeKeeperOut);

    FunctionCodeBlock* baselineCodeBlockFor(CodeSpecializationKind);
        
    FunctionCodeBlock* profiledCodeBlockFor(CodeSpecializationKind kind)
    {
        return baselineCodeBlockFor(kind);
    }

    // TSAN code-lifecycle (section 3.5, writer side): DO NOT add callers.
    // This is the legacy plain-store writer (WriteBarrierBase::setMayBeNull
    // -> setEarlyValue -> RawPtrTraits std::exchange, defined inline in
    // FunctionExecutableInlines.h) and it races the consume-ordered atomic
    // readers above. Its only historical caller, ScriptExecutable::
    // installCode, now publishes the m_codeBlockFor* slot with an atomic
    // release store directly (ScriptExecutable.cpp, FunctionCode case). Any
    // new writer must use the same atomic publication. The declaration is
    // kept only so the inline definition still compiles.
    FunctionCodeBlock* replaceCodeBlockWith(VM&, CodeSpecializationKind, CodeBlock*);

    RefPtr<TypeSet> returnStatementTypeSet() 
    {
        RareData& rareData = ensureRareData();
        if (!rareData.m_returnStatementTypeSet)
            rareData.m_returnStatementTypeSet = TypeSet::create();
        return rareData.m_returnStatementTypeSet;
    }
        
    FunctionMode functionMode() { return m_unlinkedExecutable->functionMode(); }
    ImplementationVisibility implementationVisibility() const { return m_unlinkedExecutable->implementationVisibility(); }
    bool isBuiltinFunction() const { return m_unlinkedExecutable->isBuiltinFunction(); }
    bool isPrivateBuiltinFunction() const { return isBuiltinFunction() && (!m_source.provider() || !m_source.provider()->sourceURL()); }
    ConstructAbility constructAbility() const { return m_unlinkedExecutable->constructAbility(); }
    InlineAttribute inlineAttribute() const { return m_unlinkedExecutable->inlineAttribute(); }
    bool isClass() const { return m_unlinkedExecutable->isClass(); }
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
    DerivedContextType derivedContextType() const { return m_unlinkedExecutable->derivedContextType(); }
    bool isClassConstructorFunction() const { return m_unlinkedExecutable->isClassConstructorFunction(); }
    const Identifier& name() { return m_unlinkedExecutable->name(); }
    const Identifier& ecmaName() { return m_unlinkedExecutable->ecmaName(); }
    unsigned parameterCount() const { return m_unlinkedExecutable->parameterCount(); } // Excluding 'this'!
    SourceParseMode parseMode() const { return m_unlinkedExecutable->parseMode(); }
    JSParserScriptMode scriptMode() const { return m_unlinkedExecutable->scriptMode(); }
    SourceCode classSource() const { return m_unlinkedExecutable->classSource(); }

    DECLARE_VISIT_CHILDREN;
    DECLARE_VISIT_OUTPUT_CONSTRAINTS;
    inline static Structure* createStructure(VM&, JSGlobalObject*, JSValue);

    static constexpr int overrideLineNumberNotFound = -1;
    void setOverrideLineNumber(int overrideLineNumber)
    {
        if (overrideLineNumber == overrideLineNumberNotFound) {
            if (RareData* rareData = rareDataConcurrently()) [[unlikely]]
                rareData->m_overrideLineNumber = std::nullopt;
            return;
        }
        ensureRareData().m_overrideLineNumber = overrideLineNumber;
    }

    std::optional<int> overrideLineNumber() const
    {
        if (RareData* rareData = rareDataConcurrently()) [[unlikely]]
            return rareData->m_overrideLineNumber;
        return std::nullopt;
    }

    int lineCount() const
    {
        if (RareData* rareData = rareDataConcurrently()) [[unlikely]]
            return rareData->m_lineCount;
        return m_unlinkedExecutable->lineCount();
    }

    int endColumn() const
    {
        if (RareData* rareData = rareDataConcurrently()) [[unlikely]]
            return rareData->m_endColumn;
        return m_unlinkedExecutable->linkedEndColumn(m_source.startColumn().oneBasedInt());
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
        if (RareData* rareData = rareDataConcurrently()) [[unlikely]]
            return rareData->m_functionEnd;
        return m_unlinkedExecutable->unlinkedFunctionEnd();
    }

    unsigned functionStart() const
    {
        if (RareData* rareData = rareDataConcurrently()) [[unlikely]]
            return rareData->m_functionStart;
        return m_unlinkedExecutable->unlinkedFunctionStart();
    }

    unsigned parametersStartOffset() const
    {
        if (RareData* rareData = rareDataConcurrently()) [[unlikely]]
            return rareData->m_parametersStartOffset;
        return m_unlinkedExecutable->parametersStartOffset();
    }

    void overrideInfo(const FunctionOverrideInfo&);

    DECLARE_EXPORT_INFO;

    InferredValue<JSFunction>& singleton()
    {
        return m_singleton;
    }

    void notifyCreation(VM& vm, JSFunction* function, const char* reason)
    {
        m_singleton.notifyWrite(vm, this, function, reason);
        if (m_singleton.hasBeenInvalidated())
            m_unlinkedExecutable->setSingletonHasBeenInvalidated();
    }

    // Cached poly proto structure for the result of constructing this executable.
    Structure* cachedPolyProtoStructure()
    {
        if (RareData* rareData = rareDataConcurrently()) [[unlikely]]
            return rareData->m_cachedPolyProtoStructureID.get();
        return nullptr;
    }
    void setCachedPolyProtoStructure(VM& vm, Structure* structure)
    {
        ensureRareData().m_cachedPolyProtoStructureID.set(vm, this, structure);
    }

    InlineWatchpointSet& ensurePolyProtoWatchpoint()
    {
        // GIL-off (TSAN r11 report 22, REAL create-once race — same shape as
        // FunctionExecutable::ensureRareDataSlow): two Threads racing the
        // unserialized create assigned the RefPtr twice; the losing
        // assignment destroyed a set the winner may already have handed out
        // (registered watchpoints on a dead set = UAF shape), and the plain
        // pointer write raced concurrent readers. Fast path: one acquire
        // probe of the single pointer word (Box is exactly one RefPtr);
        // creation is serialized under cellLock() in the slow path, which
        // release-publishes the same word. Once non-null the word is
        // immutable, so the plain deref below is ordered by the acquire.
        static_assert(sizeof(Box<InlineWatchpointSet>) == sizeof(uintptr_t));
        if (WTF::atomicLoad(std::bit_cast<uintptr_t*>(&m_polyProtoWatchpoint), std::memory_order_acquire)) [[likely]]
            return *m_polyProtoWatchpoint;
        return ensurePolyProtoWatchpointSlow();
    }

    JS_EXPORT_PRIVATE InlineWatchpointSet& ensurePolyProtoWatchpointSlow();

    Box<InlineWatchpointSet> sharedPolyProtoWatchpoint() const
    {
        // See ensurePolyProtoWatchpoint: snapshot the word with an acquire
        // load so the copy cannot race the locked creator's release publish;
        // non-null is immutable, so the ordered plain RefPtr copy is safe
        // (Box's payload is ThreadSafeRefCounted).
        if (!WTF::atomicLoad(const_cast<uintptr_t*>(std::bit_cast<const uintptr_t*>(&m_polyProtoWatchpoint)), std::memory_order_acquire))
            return nullptr;
        return m_polyProtoWatchpoint;
    }

    ScriptExecutable* topLevelExecutable() const LIFETIME_BOUND { return m_topLevelExecutable.get(); }

    TemplateObjectMap& ensureTemplateObjectMap(VM&);

    void finalizeUnconditionally(VM&, CollectionScope);

    JSString* toString(JSGlobalObject*);
    JSString* asStringConcurrently() const
    {
        if (!rareDataConcurrently())
            return nullptr;
        return rareDataConcurrently()->m_asString.get();
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
    };

private:
    friend class ExecutableBase;
    FunctionExecutable(VM&, ScriptExecutable* topLevelExecutable, const SourceCode&, UnlinkedFunctionExecutable*, Intrinsic, bool isInsideOrdinaryFunction);

    DECLARE_DEFAULT_FINISH_CREATION;

    friend class ScriptExecutable;

    RareData& ensureRareData()
    {
        // THREADS: acquire-load the lazily published rare data — two Threads
        // can race here; creation is serialized under cellLock() in the slow
        // path (a plain double-create would delete the loser's RareData while
        // the winner's caller is already using it).
        if (RareData* rareData = rareDataConcurrently()) [[likely]]
            return *rareData;
        return ensureRareDataSlow();
    }
    RareData* rareDataConcurrently() const
    {
        return WTF::atomicLoad(std::bit_cast<RareData**>(const_cast<std::unique_ptr<RareData>*>(&m_rareData)), std::memory_order_acquire);
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
    Box<InlineWatchpointSet> m_polyProtoWatchpoint;
};

} // namespace JSC
