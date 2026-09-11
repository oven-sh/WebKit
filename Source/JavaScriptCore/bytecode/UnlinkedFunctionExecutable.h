/*
 * Copyright (C) 2012-2022 Apple Inc. All rights reserved.
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

#include "CodeSpecializationKind.h"
#include "DeclaredNamesLink.h"
#include "ConstructAbility.h"
#include "ConstructorKind.h"
#include "ExecutableInfo.h"
#include "Identifier.h"
#include "ImplementationVisibility.h"
#include "InlineAttribute.h"
#include "Intrinsic.h"
#include "JSCast.h"
#include "ParserModes.h"
#include "ParserTokens.h"
#include "RegExp.h"
#include "SourceCode.h"
#include "VariableEnvironment.h"
#include <wtf/FixedVector.h>
#include <wtf/TZoneMalloc.h>

namespace JSC {

class CachedFunctionExecutable;
class Decoder;
class FunctionExecutable;
class FunctionMetadataNode;
class ParserError;
class ScriptExecutable;
class SourceProvider;
class UnlinkedCodeBlock;
class UnlinkedFunctionCodeBlock;

enum UnlinkedFunctionKind {
    UnlinkedNormalFunction,
    UnlinkedBuiltinFunction,
};

class UnlinkedFunctionExecutable final : public JSCell {
public:
    friend class CodeCache;
    friend class VM;
    friend class CachedFunctionExecutable;

    typedef JSCell Base;
    static constexpr unsigned StructureFlags = Base::StructureFlags | StructureIsImmortal;

    template<typename CellType, SubspaceAccess>
    static GCClient::IsoSubspace* subspaceFor(VM& vm)
    {
        return &vm.unlinkedFunctionExecutableSpace();
    }

    static UnlinkedFunctionExecutable* create(VM& vm, const SourceCode& source, FunctionMetadataNode* node, UnlinkedFunctionKind unlinkedFunctionKind, ConstructAbility constructAbility, InlineAttribute inlineAttribute, JSParserScriptMode scriptMode, RefPtr<TDZEnvironmentLink> parentScopeTDZVariables, Vector<Identifier>&& generatorOrAsyncWrapperFunctionParameterNames, std::optional<PrivateNameEnvironment> parentPrivateNameEnvironment, DerivedContextType derivedContextType, EvalContextType evalContextType, NeedsClassFieldInitializer needsClassFieldInitializer, PrivateBrandRequirement privateBrandRequirement, bool isBuiltinDefaultClassConstructor = false)
    {
        UnlinkedFunctionExecutable* instance = new (NotNull, allocateCell<UnlinkedFunctionExecutable>(vm))
            UnlinkedFunctionExecutable(vm, vm.unlinkedFunctionExecutableStructure.get(), source, node, unlinkedFunctionKind, constructAbility, inlineAttribute, scriptMode, WTF::move(parentScopeTDZVariables), WTF::move(generatorOrAsyncWrapperFunctionParameterNames), WTF::move(parentPrivateNameEnvironment), derivedContextType, evalContextType, needsClassFieldInitializer, privateBrandRequirement, isBuiltinDefaultClassConstructor);
        instance->finishCreation(vm);
        return instance;
    }

    ~UnlinkedFunctionExecutable();

    const Identifier& name() const;
    const Identifier& ecmaName() const
    {
        if (m_nameIsDeferred) [[unlikely]]
            materializeDeferredNameSlow();
        return m_ecmaName;
    }
    // Like JSFunction::nameWithoutGC(): also for the stack traces the collector's end phase builds
    // (ErrorInstance::computeErrorInfo), where nothing may be atomized, so there a name still in the bytecode cache is
    // copied out of it instead of being materialized.
    String ecmaNameWithoutGC() const
    {
        if (m_nameIsDeferred) [[unlikely]]
            return ecmaNameWithoutGCSlow();
        return m_ecmaName.string();
    }
    String nameWithoutGC() const { return m_hasName ? ecmaNameWithoutGC() : String(); }
    // For threads other than the mutator (compiler-thread dumps): null while the name is still only in the bytecode cache.
    const Identifier* tryGetEcmaNameConcurrently() const
    {
        if (WTF::atomicLoad(const_cast<bool*>(&m_nameIsDeferred), std::memory_order_acquire))
            return nullptr;
        return &m_ecmaName;
    }
    void setEcmaName(const Identifier& name)
    {
        if (m_nameIsDeferred) [[unlikely]]
            materializeDeferredNameSlow();
        ASSERT(!m_hasName || name == m_ecmaName);
        m_ecmaName = name;
    }
    unsigned parameterCount() const { return m_parameterCount; }; // Excluding 'this'!
    SourceParseMode parseMode() const { return static_cast<SourceParseMode>(m_sourceParseMode); };

    SourceCode classSource() const
    {
        materializeDeferredMembersIfNeeded();
        if (m_members.live().rareData)
            return m_members.live().rareData->m_classSource;
        return SourceCode();
    }
    void setClassSource(const SourceCode& source)
    {
        ensureRareData().m_classSource = source;
        m_isClass = !source.isNull();
    }

    // Options::useThinChildExecutables(): an executable decoded from a bytecode cache leaves its ecmaName, parent scope
    // TDZ variables and rare data in the cache until something asks for them (name reflection, generating bytecode for
    // it, toString of a class, re-encoding). Only the mutator materializes; calling the function does not, so compiler
    // and GC threads use tryGetEcmaNameConcurrently() (FunctionExecutable::inferredNameForTools()).
    void materializeDeferredMembersIfNeeded() const
    {
        if (m_membersAreDeferred) [[unlikely]]
            materializeDeferredMembersSlow();
    }
    // Likewise for the source positions only introspection reads (toString, debugger, profilers, FunctionExecutable
    // rare data): line count, parameters start, function end, body end column -- CachedFunctionExecutable's cold tail.
    // Calling the function does not need them either.
    void materializeDeferredScalarsIfNeeded() const
    {
        if (m_scalarsAreDeferred) [[unlikely]]
            materializeDeferredScalarsSlow();
    }

    bool isInStrictContext() const { return m_lexicallyScopedFeatures & StrictModeLexicallyScopedFeature; }
    FunctionMode functionMode() const { return static_cast<FunctionMode>(m_functionMode); }
    ConstructorKind constructorKind() const { return static_cast<ConstructorKind>(m_constructorKind); }
    SuperBinding superBinding() const { return static_cast<SuperBinding>(m_superBinding); }

    unsigned lineCount() const { materializeDeferredScalarsIfNeeded(); return m_lineCount; }
    unsigned linkedStartColumn(unsigned parentStartColumn) const { return m_unlinkedBodyStartColumn + (!m_firstLineOffset ? parentStartColumn : 1); }
    unsigned linkedEndColumn(unsigned startColumn) const { materializeDeferredScalarsIfNeeded(); return m_unlinkedBodyEndColumn + (!m_lineCount ? startColumn : 1); }

    unsigned unlinkedFunctionStart() const { return m_unlinkedFunctionStart; }
    unsigned unlinkedFunctionEnd() const { materializeDeferredScalarsIfNeeded(); return m_unlinkedFunctionEnd; }
    unsigned unlinkedBodyStartColumn() const { return m_unlinkedBodyStartColumn; }
    unsigned unlinkedBodyEndColumn() const { materializeDeferredScalarsIfNeeded(); return m_unlinkedBodyEndColumn; }
    unsigned startOffset() const { return m_startOffset; }
    unsigned sourceLength() { return m_sourceLength; }
    unsigned parametersStartOffset() const { materializeDeferredScalarsIfNeeded(); return m_parametersStartOffset; }

    UnlinkedFunctionCodeBlock* unlinkedCodeBlockFor(
        VM&, const SourceCode&, CodeSpecializationKind, OptionSet<CodeGenerationMode>,
        ParserError&, SourceParseMode, OptimizeBytecode = OptimizeBytecode::No);

    static UnlinkedFunctionExecutable* fromGlobalCode(
        const Identifier&, JSGlobalObject*, const SourceCode&, LexicallyScopedFeatures, JSObject*& exception,
        int overrideLineNumber, std::optional<int> functionConstructorParametersEndPosition);

    SourceCode linkedSourceCode(const SourceCode&) const;
    JS_EXPORT_PRIVATE FunctionExecutable* link(VM&, ScriptExecutable* topLevelExecutable, const SourceCode& parentSource, std::optional<int> overrideLineNumber = std::nullopt, Intrinsic = NoIntrinsic, bool isInsideOrdinaryFunction = false);

    void clearCode(VM& vm)
    {
        ASSERT(!m_isCached);
        m_unlinkedCodeBlockForCall.clear();
        m_unlinkedCodeBlockForConstruct.clear();
        // FIXME GlobalGC: Need syncrhonization here for accessing the Heap server.
        vm.heap.unlinkedFunctionExecutableSpaceAndSet.set.remove(this);
    }

    // If every code block this holds was decoded from a persistent bytecode cache payload, lets go of them and goes
    // back to naming their records in that payload, as before the first call; the next unlinkedCodeBlockFor() decodes
    // them again. Only with no collection and no compiler thread running (Heap::deleteAllUnlinkedCodeBlocks).
    // (Not if one of them is in `linkedAgainst`.)
    bool returnCodeToCache(VM&, const UncheckedKeyHashSet<UnlinkedCodeBlock*>& linkedAgainst);

    void recordParse(CodeFeatures features, LexicallyScopedFeatures lexicallyScopedFeatures, bool hasCapturedVariables)
    {
        m_features = features;
        m_lexicallyScopedFeatures = lexicallyScopedFeatures;
        m_hasCapturedVariables = hasCapturedVariables;
    }

    CodeFeatures features() const { return m_features; }
    LexicallyScopedFeatures lexicallyScopedFeatures() const { return m_lexicallyScopedFeatures; }
    bool hasCapturedVariables() const { return m_hasCapturedVariables; }

    PrivateBrandRequirement privateBrandRequirement() const { return static_cast<PrivateBrandRequirement>(m_privateBrandRequirement); }

    static constexpr DestructionMode needsDestruction = NeedsDestruction;
    static void destroy(JSCell*);

    ImplementationVisibility implementationVisibility() const { return static_cast<ImplementationVisibility>(m_implementationVisibility); }
    bool isBuiltinFunction() const { return m_isBuiltinFunction; }
    // The code blocks are still (or again) in the bytecode cache this was decoded from.
    bool isCached() const { return m_isCached; }
    ConstructAbility constructAbility() const { return static_cast<ConstructAbility>(m_constructAbility); }
    JSParserScriptMode scriptMode() const { return static_cast<JSParserScriptMode>(m_scriptMode); }
    bool isClassConstructorFunction() const
    {
        switch (constructorKind()) {
        case ConstructorKind::None:
        case ConstructorKind::Naked:
            return false;
        case ConstructorKind::Base:
        case ConstructorKind::Extends:
            return true;
        }
        return false;
    }
    bool isClass() const { return m_isClass; }
    bool isBuiltinDefaultClassConstructor() const { return m_isBuiltinDefaultClassConstructor; }

    RefPtr<TDZEnvironmentLink> parentScopeTDZVariables() const
    {
        materializeDeferredMembersIfNeeded();
        return m_members.live().parentScopeTDZVariables;
    }
    void setParentDeclaredNames(RefPtr<DeclaredNamesLink>&& names) { materializeDeferredMembersIfNeeded(); ensureRareData().m_parentDeclaredNames = WTF::move(names); }
    // Taken by the first code block generated for this executable (call or construct); a second specialization of
    // the same function is generated without static scope information.
    RefPtr<DeclaredNamesLink> takeParentDeclaredNames()
    {
        materializeDeferredMembersIfNeeded();
        if (!m_members.live().rareData || !m_members.live().rareData->m_parentDeclaredNames)
            return nullptr;
        RefPtr<DeclaredNamesLink> result = std::exchange(m_members.live().rareData->m_parentDeclaredNames, nullptr);
        if (m_members.live().rareData->isEmpty())
            m_members.live().rareData = nullptr;
        return result;
    }

    const FixedVector<Identifier>* generatorOrAsyncWrapperFunctionParameterNames() const
    {
        materializeDeferredMembersIfNeeded();
        if (!m_members.live().rareData)
            return nullptr;
        return &m_members.live().rareData->m_generatorOrAsyncWrapperFunctionParameterNames;
    }

    const PrivateNameEnvironment* parentPrivateNameEnvironment() const
    {
        materializeDeferredMembersIfNeeded();
        if (!m_members.live().rareData)
            return nullptr;
        return &m_members.live().rareData->m_parentPrivateNameEnvironment;
    }
    
    bool isArrowFunction() const { return isArrowFunctionParseMode(parseMode()); }

    bool singletonHasBeenInvalidated() const { return m_singletonHasBeenInvalidated; }
    void setSingletonHasBeenInvalidated() { m_singletonHasBeenInvalidated = true; }

    JSC::DerivedContextType derivedContextType() const {return static_cast<JSC::DerivedContextType>(m_derivedContextType); }
    EvalContextType evalContextType() const { return static_cast<EvalContextType>(m_evalContextType); }

    InlineAttribute inlineAttribute() const { return static_cast<InlineAttribute>(m_inlineAttribute); }

    String sourceURLDirective() const
    {
        materializeDeferredMembersIfNeeded();
        if (m_members.live().rareData)
            return m_members.live().rareData->m_sourceURLDirective;
        return String();
    }
    String sourceMappingURLDirective() const
    {
        materializeDeferredMembersIfNeeded();
        if (m_members.live().rareData)
            return m_members.live().rareData->m_sourceMappingURLDirective;
        return String();
    }
    void setSourceURLDirective(const String& sourceURL)
    {
        ensureRareData().m_sourceURLDirective = sourceURL;
    }
    void setSourceMappingURLDirective(const String& sourceMappingURL)
    {
        ensureRareData().m_sourceMappingURLDirective = sourceMappingURL;
    }

    void reconcileWeakReferencesAtGCEnd(VM&, CollectionScope);

    struct ClassElementDefinition {
        WTF_MAKE_STRUCT_TZONE_ALLOCATED(ClassElementDefinition);

        enum class Kind : uint8_t {
            FieldWithLiteralPropertyKey = 0,
            FieldWithComputedPropertyKey = 1,
            FieldWithPrivatePropertyKey = 2,
            StaticInitializationBlock = 3,
        };

        Identifier ident { };
        JSTextPosition position { };
        std::optional<JSTextPosition> initializerPosition { std::nullopt };
        Kind kind { Kind::FieldWithLiteralPropertyKey };
    };

    struct RareData {
        WTF_MAKE_STRUCT_TZONE_ALLOCATED(RareData);

        SourceCode m_classSource;
        String m_sourceURLDirective;
        String m_sourceMappingURLDirective;
        FixedVector<Identifier> m_generatorOrAsyncWrapperFunctionParameterNames;
        FixedVector<ClassElementDefinition> m_classElementDefinitions;
        PrivateNameEnvironment m_parentPrivateNameEnvironment;
        // Only while generating with OptimizeBytecode::Yes and only until this executable's code is generated: the
        // enclosing scopes at the creation site. Never encoded into a bytecode cache.
        RefPtr<DeclaredNamesLink> m_parentDeclaredNames;

        bool isEmpty() const
        {
            return m_classSource.isNull() && m_sourceURLDirective.isNull() && m_sourceMappingURLDirective.isNull()
                && m_generatorOrAsyncWrapperFunctionParameterNames.isEmpty() && m_classElementDefinitions.isEmpty()
                && m_parentPrivateNameEnvironment.isEmpty() && !m_parentDeclaredNames;
        }
    };

    NeedsClassFieldInitializer needsClassFieldInitializer() const { return static_cast<NeedsClassFieldInitializer>(m_needsClassFieldInitializer); }

    const FixedVector<ClassElementDefinition>* classElementDefinitions() const
    {
        materializeDeferredMembersIfNeeded();
        if (m_members.live().rareData)
            return &m_members.live().rareData->m_classElementDefinitions;
        return nullptr;
    }

    void setClassElementDefinitions(Vector<ClassElementDefinition>&& classElementDefinitions)
    {
        if (classElementDefinitions.isEmpty())
            return;
        ensureRareData().m_classElementDefinitions = FixedVector<ClassElementDefinition>(WTF::move(classElementDefinitions));
    }

private:
    UnlinkedFunctionExecutable(VM&, Structure*, const SourceCode&, FunctionMetadataNode*, UnlinkedFunctionKind, ConstructAbility, InlineAttribute, JSParserScriptMode, RefPtr<TDZEnvironmentLink>, Vector<Identifier>&&, std::optional<PrivateNameEnvironment>, JSC::DerivedContextType, EvalContextType, JSC::NeedsClassFieldInitializer, PrivateBrandRequirement, bool isBuiltinDefaultClassConstructor);
    UnlinkedFunctionExecutable(Decoder&, const CachedFunctionExecutable&);

    DECLARE_VISIT_CHILDREN;

    void decodeCachedCodeBlocks(VM&);
    JS_EXPORT_PRIVATE void materializeDeferredNameSlow() const;
    JS_EXPORT_PRIVATE String ecmaNameWithoutGCSlow() const;
    JS_EXPORT_PRIVATE void materializeDeferredMembersSlow() const;
    JS_EXPORT_PRIVATE void materializeDeferredScalarsSlow() const;

    bool codeBlockEdgeMayBeWeak() const
    {
        // Currently, bytecode cache assumes that the tree of UnlinkedFunctionExecutable and UnlinkedCodeBlock will not be destroyed while the parent is live.
        // Bytecode cache uses this asumption to avoid duplicate materialization by bookkeeping the heap cells in the offste-to-pointer map.
        return VM::useUnlinkedCodeBlockJettisoning() && !m_isGeneratedFromCache;
    }

    unsigned m_firstLineOffset : 31;
    unsigned m_isGeneratedFromCache : 1;
    unsigned m_lineCount : 31;
    unsigned m_hasCapturedVariables : 1;
    unsigned m_unlinkedFunctionStart: 31;
    unsigned m_isBuiltinFunction : 1;
    unsigned m_unlinkedBodyStartColumn : 31;
    unsigned m_isBuiltinDefaultClassConstructor : 1;
    // m_lineCount, m_unlinkedBodyEndColumn, m_parametersStartOffset and m_unlinkedFunctionEnd may be written late
    // (m_scalarsAreDeferred); the bit each shares its word with is one only the mutator reads.
    unsigned m_unlinkedBodyEndColumn : 31;
    unsigned m_superBinding : 1;
    unsigned m_startOffset : 31;
    unsigned m_isCached : 1;
    unsigned m_sourceLength : 31;
    unsigned m_constructAbility: 1;
    unsigned m_parametersStartOffset : 31;
    unsigned m_scriptMode: 1; // JSParserScriptMode
    unsigned m_unlinkedFunctionEnd : 31;
    unsigned m_needsClassFieldInitializer : 1;
    unsigned m_parameterCount : 30;
    unsigned m_singletonHasBeenInvalidated : 1;
    unsigned m_privateBrandRequirement : 1;
    CodeFeatures m_features : bitWidthOfCodeFeatures;
    uint16_t m_constructorKind : 2;
    SourceParseMode m_sourceParseMode;
    uint8_t m_implementationVisibility : bitWidthOfImplementationVisibility;
    LexicallyScopedFeatures m_lexicallyScopedFeatures : bitWidthOfLexicallyScopedFeatures;
    uint8_t m_functionMode : 2; // FunctionMode
    uint8_t m_derivedContextType : 2;
    uint8_t m_inlineAttribute : 1;
    uint8_t m_evalContextType : 2;
    uint8_t m_hasName : 1;
    uint8_t m_isClass : 1;
    // Own bytes, not bits of the group above: the mutator clears these late, while compiler threads read that group.
    bool m_nameIsDeferred { false }; // m_ecmaName is still in the cache record; implies m_membersAreDeferred
    bool m_membersAreDeferred { false }; // TDZ variables + rare data are still in the cache record; the m_deferredMembers* union members are live
    bool m_scalarsAreDeferred { false }; // the record's cold tail was not read yet (those four members are 0); implies m_membersAreDeferred (that state holds the record)

    union {
        WriteBarrier<UnlinkedFunctionCodeBlock> m_unlinkedCodeBlockForCall;
        RefPtr<Decoder> m_decoder;
    };

    // While m_isCached, where each code block is in m_decoder's payload: > 0 is the offset of this executable's slot
    // for it in its own record (as decoded), < 0 the negated offset of the code block's record (returnCodeToCache()).
    union {
        WriteBarrier<UnlinkedFunctionCodeBlock> m_unlinkedCodeBlockForConstruct;
        struct {
            int32_t m_cachedCodeBlockForCallOffset;
            int32_t m_cachedCodeBlockForConstructOffset;
        };
    };

    Identifier m_ecmaName;

    // parentScopeTDZVariables and rareData, or, while m_membersAreDeferred, the cache record they still live in.
    class DeferredMembers {
    public:
        struct Live {
            RefPtr<TDZEnvironmentLink> parentScopeTDZVariables;
            std::unique_ptr<RareData> rareData;
        };
        struct Pending {
            RefPtr<Decoder> decoder;
            const CachedFunctionExecutable* record; // in decoder's payload
        };
        explicit DeferredMembers(RefPtr<TDZEnvironmentLink>&& parentScopeTDZVariables) { new (&m_live) Live { WTF::move(parentScopeTDZVariables), nullptr }; }
        ~DeferredMembers();
        bool isPending() const;
        Live& live() { ASSERT(!isPending()); return m_live; }
        const Live& live() const { ASSERT(!isPending()); return m_live; }
        const Pending& pending() const { ASSERT(isPending()); return m_pending; }
        void defer(Decoder&, const CachedFunctionExecutable&); // empty Live -> Pending
        void settle(Live&&); // Pending -> Live, then clears the owner's flag
    private:
        UnlinkedFunctionExecutable& owner() const;
        union {
            Live m_live;
            Pending m_pending;
        };
    };
    DeferredMembers m_members;

    RareData& ensureRareData()
    {
        materializeDeferredMembersIfNeeded();
        if (m_members.live().rareData) [[likely]]
            return *m_members.live().rareData;
        return ensureRareDataSlow();
    }
    RareData& ensureRareDataSlow();

public:
    inline static Structure* createStructure(VM&, JSGlobalObject*, JSValue);

    DECLARE_EXPORT_INFO;
};

inline UnlinkedFunctionExecutable& UnlinkedFunctionExecutable::DeferredMembers::owner() const
{
    return *std::bit_cast<UnlinkedFunctionExecutable*>(std::bit_cast<uintptr_t>(this) - OBJECT_OFFSETOF(UnlinkedFunctionExecutable, m_members));
}

inline bool UnlinkedFunctionExecutable::DeferredMembers::isPending() const { return owner().m_membersAreDeferred; }

inline UnlinkedFunctionExecutable::DeferredMembers::~DeferredMembers()
{
    if (isPending())
        m_pending.~Pending();
    else
        m_live.~Live();
}

inline void UnlinkedFunctionExecutable::DeferredMembers::defer(Decoder& decoder, const CachedFunctionExecutable& record)
{
    ASSERT(!isPending() && !m_live.parentScopeTDZVariables && !m_live.rareData);
    m_live.~Live();
    new (&m_pending) Pending { &decoder, &record };
    owner().m_membersAreDeferred = true;
}

inline void UnlinkedFunctionExecutable::DeferredMembers::settle(Live&& live)
{
    ASSERT(isPending());
    m_pending.~Pending();
    new (&m_live) Live(WTF::move(live));
    owner().m_membersAreDeferred = false;
}

#if !ASSERT_ENABLED
static_assert(sizeof(UnlinkedFunctionExecutable) <= 96, "UnlinkedFunctionExecutable needs to be small");
#endif

} // namespace JSC
