/*
 * Copyright (C) 2009-2019 Apple Inc. All rights reserved.
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

#include "ExecutableBase.h"
#include "Intrinsic.h"
#include "ParserModes.h"
#include "ProfilerJettisonReason.h"
#include <wtf/Atomics.h>

namespace JSC {

class JSArray;
class JSTemplateObjectDescriptor;
class IsoCellSet;

class ScriptExecutable : public ExecutableBase {
public:
    typedef ExecutableBase Base;
    static constexpr unsigned StructureFlags = Base::StructureFlags;

    static void destroy(JSCell*);

    using TemplateObjectMap = UncheckedKeyHashMap<uint64_t, WriteBarrier<JSArray>, WTF::IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>>;
        
    CodeBlockHash hashFor(CodeSpecializationKind) const;

    const SourceCode& source() const LIFETIME_BOUND { return m_source; }
    SourceID sourceID() const { return m_source.providerID(); }
    const SourceOrigin& sourceOrigin() const LIFETIME_BOUND { return m_source.provider()->sourceOrigin(); }
    // This is NOT the path that should be used for computing relative paths from a script. Use SourceOrigin's URL for that, the values may or may not be the same... This should only be used for `error.sourceURL` and stack traces.
    const String& sourceURL() const LIFETIME_BOUND { return m_source.provider()->sourceURL(); }
    const String& sourceURLStripped() const LIFETIME_BOUND { return m_source.provider()->sourceURLStripped(); }
    const String& preRedirectURL() const LIFETIME_BOUND { return m_source.provider()->preRedirectURL(); }
    int firstLine() const { return m_source.firstLine().oneBasedInt(); }
    JS_EXPORT_PRIVATE int NODELETE lastLine() const;
    unsigned startColumn() const { return m_source.startColumn().oneBasedInt(); }
    JS_EXPORT_PRIVATE unsigned NODELETE endColumn() const;

    std::optional<int> NODELETE overrideLineNumber(VM&) const;
    unsigned NODELETE typeProfilingStartOffset() const;
    unsigned NODELETE typeProfilingEndOffset() const;

    bool usesArguments() const { return featuresConcurrently() & ArgumentsFeature; }
    bool isArrowFunctionContext() const { return m_isArrowFunctionContext; }
    DerivedContextType derivedContextType() const { return static_cast<DerivedContextType>(m_derivedContextType); }
    EvalContextType evalContextType() const { return static_cast<EvalContextType>(m_evalContextType); }
    bool isInStrictContext() const { return lexicallyScopedFeaturesConcurrently() & StrictModeLexicallyScopedFeature; }
    bool usesNonSimpleParameterList() const { return featuresConcurrently() & NonSimpleParameterListFeature; }

    // THREADS §5.7 (racy-profiling tolerance): m_neverInline and m_didTryToEnterInLoop
    // are advisory flags written from optimization slow paths that can now run on any of
    // N mutators (and read by concurrent compiler threads). All C++ accesses are relaxed
    // atomics on dedicated (non-bitfield) bytes so the tolerated race is not UB; a lost
    // or stale flag at worst delays/repeats an optimization decision, never breaks
    // soundness. m_didTryToEnterInLoop is additionally stored to directly by LLInt/JIT'd
    // code via addressOfDidTryToEnterInLoop() (plain byte store, allowed to stay plain).
    void setNeverInline(bool value) { WTF::atomicStore(&m_neverInline, value, std::memory_order_relaxed); }
    void setNeverOptimize(bool value) { m_neverOptimize = value; }
    void setNeverFTLOptimize(bool value) { m_neverFTLOptimize = value; }
    void setDidTryToEnterInLoop(bool value) { WTF::atomicStore(&m_didTryToEnterInLoop, value, std::memory_order_relaxed); }
    void setCanUseOSRExitFuzzing(bool value) { m_canUseOSRExitFuzzing = value; }
    bool neverInline() const { return WTF::atomicLoad(const_cast<bool*>(&m_neverInline), std::memory_order_relaxed); }
    bool neverOptimize() const { return m_neverOptimize; }
    bool neverFTLOptimize() const { return m_neverFTLOptimize; }
    bool didTryToEnterInLoop() const { return WTF::atomicLoad(const_cast<bool*>(&m_didTryToEnterInLoop), std::memory_order_relaxed); }
    bool isInliningCandidate() const { return !neverInline(); }
    bool isOkToOptimize() const { return !neverOptimize(); }
    bool canUseOSRExitFuzzing() const { return m_canUseOSRExitFuzzing; }
    bool isInsideOrdinaryFunction() const { return m_isInsideOrdinaryFunction; }
    
    bool* addressOfDidTryToEnterInLoop() LIFETIME_BOUND { return &m_didTryToEnterInLoop; }

    CodeFeatures features() const { return featuresConcurrently(); }
    LexicallyScopedFeatures lexicallyScopedFeatures() { return lexicallyScopedFeaturesConcurrently(); }
    void setTaintedByWithScope() { WTF::atomicStore(&m_lexicallyScopedFeatures, static_cast<LexicallyScopedFeatures>(lexicallyScopedFeaturesConcurrently() | TaintedByWithScopeLexicallyScopedFeature), std::memory_order_relaxed); }

    // THREADS (TSAN family codeblock-init): recordParse on the parsing thread
    // races readers (isInStrictContext etc.) on other mutators/compiler
    // threads; these words are dedicated bytes (m_lexicallyScopedFeatures was
    // pulled out of its bit-field) accessed with relaxed atomics. Stale reads
    // only affect parse-derived metadata that the surrounding protocol
    // revalidates; codegen is unchanged (plain mov).
    CodeFeatures featuresConcurrently() const { return WTF::atomicLoad(const_cast<CodeFeatures*>(&m_features), std::memory_order_relaxed); }
    LexicallyScopedFeatures lexicallyScopedFeaturesConcurrently() const { return WTF::atomicLoad(const_cast<LexicallyScopedFeatures*>(&m_lexicallyScopedFeatures), std::memory_order_relaxed); }
        
    DECLARE_EXPORT_INFO;

    void NODELETE recordParse(CodeFeatures, LexicallyScopedFeatures, bool hasCapturedVariables, int lastLine, unsigned endColumn);
    void installCode(CodeBlock*);
    void installCode(VM&, CodeBlock*, CodeType, CodeSpecializationKind, Profiler::JettisonReason);
    CodeBlock* newCodeBlockFor(CodeSpecializationKind, JSFunction*, JSScope*);
    CodeBlock* newReplacementCodeBlockFor(CodeSpecializationKind);

    void clearCode(IsoCellSet&);

    Intrinsic intrinsic() const
    {
        return m_intrinsic;
    }

    bool hasJITCodeForCall() const
    {
        return m_jitCodeForCall;
    }
    bool hasJITCodeForConstruct() const
    {
        return m_jitCodeForConstruct;
    }

    // This function has an interesting GC story. Callers of this function are asking us to create a CodeBlock
    // that is not jettisoned before this function returns. Callers are essentially asking for a strong reference
    // to the CodeBlock. Because the Executable may be allocating the CodeBlock, we require callers to pass in
    // their CodeBlock*& reference because it's safe for CodeBlock to be jettisoned if Executable is the only thing
    // to point to it. This forces callers to have a CodeBlock* in a register or on the stack that will be marked
    // by conservative GC if a GC happens after we create the CodeBlock.
    template <typename ExecutableType>
    void prepareForExecution(VM&, JSFunction*, JSScope*, CodeSpecializationKind, CodeBlock*&);

    ScriptExecutable* NODELETE topLevelExecutable();
    JSArray* createTemplateObject(JSGlobalObject*, JSTemplateObjectDescriptor*);

private:
    friend class ExecutableBase;
    void prepareForExecutionImpl(VM&, JSFunction*, JSScope*, CodeSpecializationKind, CodeBlock*&);

    bool NODELETE hasClearableCode() const;

    TemplateObjectMap& ensureTemplateObjectMap(VM&);

protected:
    ScriptExecutable(Structure*, VM&, const SourceCode&, LexicallyScopedFeatures, DerivedContextType, bool isInArrowFunctionContext, bool isInsideOrdinaryFunction, EvalContextType, Intrinsic);

    void recordParse(CodeFeatures features, LexicallyScopedFeatures lexicallyScopedFeatures, bool hasCapturedVariables)
    {
        WTF::atomicStore(&m_features, features, std::memory_order_relaxed); // THREADS: see featuresConcurrently().
        WTF::atomicStore(&m_lexicallyScopedFeatures, lexicallyScopedFeatures, std::memory_order_relaxed);
        WTF::atomicStore(&m_hasCapturedVariables, hasCapturedVariables, std::memory_order_relaxed);
    }

    static TemplateObjectMap& ensureTemplateObjectMapImpl(std::unique_ptr<TemplateObjectMap>& dest);

    template<typename Visitor>
    static void runConstraint(const ConcurrentJSLocker&, Visitor&, CodeBlock*);
    template<typename Visitor>
    static void visitCodeBlockEdge(Visitor&, CodeBlock*);
    void finalizeCodeBlockEdge(VM&, WriteBarrier<CodeBlock>&);

    SourceCode m_source;
    Intrinsic m_intrinsic { NoIntrinsic };
    // Dedicated byte (never a bitfield): stored to by LLInt/JIT'd code via
    // addressOfDidTryToEnterInLoop() and accessed from C++ with relaxed atomics (THREADS §5.7).
    bool m_didTryToEnterInLoop { false };
    CodeFeatures m_features;
    // Dedicated byte (pulled out of the bit-field; THREADS family codeblock-init):
    // recordParse/setTaintedByWithScope write it with relaxed atomics while other
    // threads read isInStrictContext().
    LexicallyScopedFeatures m_lexicallyScopedFeatures;
    OptionSet<CodeGenerationMode> m_codeGenerationModeForGeneratorBody;
    bool m_hasCapturedVariables; // Dedicated byte (THREADS): written by recordParse with relaxed atomics.
    // Dedicated byte (not part of the adjacent bitfield): written from optimization slow
    // paths on any mutator; all C++ accesses are relaxed atomics (THREADS §5.7). Keeping it
    // out of the bitfield means a racing setNeverInline() cannot scribble sibling flag bits.
    bool m_neverInline;
    bool m_neverOptimize : 1;
    bool m_neverFTLOptimize : 1;
    bool m_isArrowFunctionContext : 1;
    bool m_canUseOSRExitFuzzing : 1;
    bool m_codeForGeneratorBodyWasGenerated : 1;
    bool m_isInsideOrdinaryFunction : 1;
    unsigned m_derivedContextType : 2; // DerivedContextType
    unsigned m_evalContextType : 2; // EvalContextType
};

} // namespace JSC
