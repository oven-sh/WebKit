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
#include "LineColumn.h"
#include "ParserModes.h"
#include "ProfilerJettisonReason.h"
#include "StaticHeap.h"
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

    // A FunctionExecutable may be no longer than what comes before m_source (see its short form). What is asked here of one that is
    // is answered from elsewhere.
    bool isShortForm() const { return isShortFunctionExecutable(); }
    // The way to everything from m_source on, once it is known to be there. (To the compiler one of these is as long as they come, and
    // what is there may be read before it is known to be wanted. This is a pointer that it knows nothing about.)
    const ScriptExecutable* inFull() const { return WTF::opaque(this); }
    ScriptExecutable* inFull() { return WTF::opaque(this); }

    const SourceCode& source() const LIFETIME_BOUND
    {
        if (isShortForm()) [[unlikely]]
            return sourceOfShortForm();
        return inFull()->m_source;
    }
    SourceProvider* sourceProvider() const
    {
        if (isShortForm()) [[unlikely]]
            return sourceProviderOfShortForm();
        return inFull()->m_source.provider();
    }
    SourceID sourceID() const
    {
        SourceProvider* provider = sourceProvider();
        return provider ? provider->asID() : SourceID();
    }
    const SourceOrigin& sourceOrigin() const LIFETIME_BOUND { return sourceProvider()->sourceOrigin(); }
#if USE(BUN_JSC_ADDITIONS)
    // A ScriptFetcher's Weak handles can ask containsOpaqueRoot(fetcher) to live as long as its code.
    template<typename Visitor> void visitSourceFetcher(Visitor& visitor) const { visitor.addOpaqueRoot(sourceOrigin().fetcher()); }
#endif
    // This is NOT the path that should be used for computing relative paths from a script. Use SourceOrigin's URL for that, the values may or may not be the same... This should only be used for `error.sourceURL` and stack traces.
    const String& sourceURL() const LIFETIME_BOUND { return sourceProvider()->sourceURL(); }
    const String& sourceURLStripped() const LIFETIME_BOUND { return sourceProvider()->sourceURLStripped(); }
    const String& preRedirectURL() const LIFETIME_BOUND { return sourceProvider()->preRedirectURL(); }
    int firstLine() const
    {
        if (isShortForm()) [[unlikely]]
            return whereShortFormStarts().line;
        return inFull()->m_source.firstLine().oneBasedInt();
    }
    JS_EXPORT_PRIVATE int NODELETE lastLine() const;
    unsigned startColumn() const
    {
        if (isShortForm()) [[unlikely]]
            return whereShortFormStarts().column;
        return inFull()->m_source.startColumn().oneBasedInt();
    }
    JS_EXPORT_PRIVATE unsigned NODELETE endColumn() const;

    std::optional<int> NODELETE overrideLineNumber(VM&) const;
    unsigned NODELETE typeProfilingStartOffset() const;
    unsigned NODELETE typeProfilingEndOffset() const;

    bool usesArguments() const { return features() & ArgumentsFeature; }
    bool isArrowFunctionContext() const { return isShortForm() ? StaticHeap::rowOf(indexOfShortForm()).isArrowFunctionContext : inFull()->m_isArrowFunctionContext; }
    DerivedContextType derivedContextType() const { return isShortForm() ? derivedContextTypeOfShortForm() : static_cast<DerivedContextType>(inFull()->m_derivedContextType); }
    EvalContextType evalContextType() const { return isShortForm() ? EvalContextType::None : static_cast<EvalContextType>(inFull()->m_evalContextType); }
    bool isInStrictContext() const { return lexicallyScopedFeatures() & StrictModeLexicallyScopedFeature; }
    bool usesNonSimpleParameterList() const { return features() & NonSimpleParameterListFeature; }

    // (There is nothing about the code of one in the short form that is anybody's to decide.)
    void setNeverInline(bool value) { if (!isShortForm()) inFull()->m_neverInline = value; }
    void setNeverOptimize(bool value) { if (!isShortForm()) inFull()->m_neverOptimize = value; }
    void setNeverFTLOptimize(bool value) { if (!isShortForm()) inFull()->m_neverFTLOptimize = value; }
    void setDidTryToEnterInLoop(bool value) { if (!isShortForm()) inFull()->m_didTryToEnterInLoop = value; }
    void setCanUseOSRExitFuzzing(bool value) { if (!isShortForm()) inFull()->m_canUseOSRExitFuzzing = value; }
    bool neverInline() const { return isShortForm() || inFull()->m_neverInline; }
    bool neverOptimize() const { return isShortForm() || inFull()->m_neverOptimize; }
    bool neverFTLOptimize() const { return isShortForm() || inFull()->m_neverFTLOptimize; }
    bool didTryToEnterInLoop() const { return !isShortForm() && inFull()->m_didTryToEnterInLoop; }
    bool isInliningCandidate() const { return !neverInline(); }
    bool isOkToOptimize() const { return !neverOptimize(); }
    bool canUseOSRExitFuzzing() const { return !isShortForm() && inFull()->m_canUseOSRExitFuzzing; }
    bool isInsideOrdinaryFunction() const { return isShortForm() ? StaticHeap::rowOf(indexOfShortForm()).isInsideOrdinaryFunction : inFull()->m_isInsideOrdinaryFunction; }
    // The code of a module: the executables of its functions are the ones that were made when the program was built.
    bool usesStaticExecutables() const { return !isShortForm() && inFull()->m_usesStaticExecutables; }
    void setUsesStaticExecutables() { inFull()->m_usesStaticExecutables = true; }
    
    bool* addressOfDidTryToEnterInLoop() LIFETIME_BOUND
    {
        RELEASE_ASSERT(!isShortForm());
        return &inFull()->m_didTryToEnterInLoop;
    }

    CodeFeatures features() const { return isShortForm() ? featuresOfShortForm() : inFull()->m_features; }
    LexicallyScopedFeatures lexicallyScopedFeatures() const { return isShortForm() ? lexicallyScopedFeaturesOfShortForm() : static_cast<LexicallyScopedFeatures>(inFull()->m_lexicallyScopedFeatures); }
    void setTaintedByWithScope()
    {
        RELEASE_ASSERT(!isShortForm());
        inFull()->m_lexicallyScopedFeatures |= TaintedByWithScopeLexicallyScopedFeature;
    }
        
    DECLARE_EXPORT_INFO;

    void NODELETE recordParse(CodeFeatures, LexicallyScopedFeatures, bool hasCapturedVariables, int lastLine, unsigned endColumn);
    void installCode(CodeBlock*);
    void installCode(VM&, CodeBlock*, CodeType, CodeSpecializationKind, Profiler::JettisonReason);
    // Code from the static compiler (aot/), for a function: it runs without a CodeBlock, and the function does not get one.
    void installAOTCode(VM&, CodeSpecializationKind, Ref<JITCode>&&);
    CodeBlock* newCodeBlockFor(CodeSpecializationKind, JSFunction*, JSScope*);
    CodeBlock* newReplacementCodeBlockFor(CodeSpecializationKind);

    // KeepWhatNeedsParsing: linked code goes; unlinked code of a program, eval or module only if a bytecode cache can
    // hand it back, and a module keeps the symbol table its environment was made from.
    enum class ClearCode : uint8_t { All, KeepWhatNeedsParsing };
    void clearCode(IsoCellSet&, ClearCode = ClearCode::All);

    Intrinsic intrinsic() const
    {
        return isShortForm() ? NoIntrinsic : inFull()->m_intrinsic;
    }

    bool hasJITCodeForCall() const
    {
        return !isShortForm() && inFull()->m_jitCodeForCall;
    }
    bool hasJITCodeForConstruct() const
    {
        return !isShortForm() && inFull()->m_jitCodeForConstruct;
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
        RELEASE_ASSERT(!isShortForm());
        m_features = features;
        m_lexicallyScopedFeatures = lexicallyScopedFeatures;
        m_hasCapturedVariables = hasCapturedVariables;
    }

    static TemplateObjectMap& ensureTemplateObjectMapImpl(std::unique_ptr<TemplateObjectMap>& dest);

    template<typename Visitor>
    static void runConstraint(const ConcurrentJSLocker&, Visitor&, CodeBlock*);
    template<typename Visitor>
    static void visitCodeBlockEdge(Visitor&, CodeBlock*);
    void jettisonCodeBlockEdgeIfDead(VM&, WriteBarrier<CodeBlock>&);

    // The body of a generator or an async function keeps its registers in a generator frame while it is suspended, and
    // only code that was generated the same way finds them there again: once such a body has run, its code is always
    // generated the way it was then, whatever the realm asks for by now. (A module keeps the mode its environment's
    // symbol table was made for: ModuleProgramExecutable::getUnlinkedCodeBlock.)
    OptionSet<CodeGenerationMode> codeGenerationModeForResumableBody(OptionSet<CodeGenerationMode> current)
    {
        if (m_codeForGeneratorBodyWasGenerated)
            return m_codeGenerationModeForGeneratorBody;
        m_codeGenerationModeForGeneratorBody = current;
        return current;
    }
    void pinCodeGenerationModeForResumableBody() { m_codeForGeneratorBodyWasGenerated = true; }

    // Which function one in the short form is (AOT::ImageFunction::index).
    uint32_t indexOfShortForm() const { return m_aotIndex[m_aotEntry[0] ? 0 : 1]; }
    JS_EXPORT_PRIVATE const SourceCode& sourceOfShortForm() const;
    JS_EXPORT_PRIVATE SourceProvider* sourceProviderOfShortForm() const;
    JS_EXPORT_PRIVATE LineColumn whereShortFormStarts() const;
    JS_EXPORT_PRIVATE CodeFeatures featuresOfShortForm() const;
    JS_EXPORT_PRIVATE LexicallyScopedFeatures lexicallyScopedFeaturesOfShortForm() const;
    JS_EXPORT_PRIVATE DerivedContextType derivedContextTypeOfShortForm() const;

    SourceCode m_source;
    Intrinsic m_intrinsic { NoIntrinsic };
    bool m_didTryToEnterInLoop { false };
    CodeFeatures m_features;
    LexicallyScopedFeatures m_lexicallyScopedFeatures : bitWidthOfLexicallyScopedFeatures;
    OptionSet<CodeGenerationMode> m_codeGenerationModeForGeneratorBody;
    bool m_hasCapturedVariables : 1;
    bool m_neverInline : 1;
    bool m_neverOptimize : 1;
    bool m_neverFTLOptimize : 1;
    bool m_isArrowFunctionContext : 1;
    bool m_canUseOSRExitFuzzing : 1;
    bool m_codeForGeneratorBodyWasGenerated : 1;
    bool m_isInsideOrdinaryFunction : 1;
    bool m_usesStaticExecutables : 1 { false };
    unsigned m_derivedContextType : 2; // DerivedContextType
    unsigned m_evalContextType : 2; // EvalContextType
};

} // namespace JSC
