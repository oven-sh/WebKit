/*
 * Copyright (C) 2012-2025 Apple Inc. All rights reserved.
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

#include "config.h"
#include "UnlinkedFunctionExecutable.h"

#include "BuiltinExecutables.h"
#include "BytecodeGenerator.h"
#include "CachedTypes.h"
#include "ClassInfo.h"
#include "CodeCache.h"
#include "Debugger.h"
#include "ExecutableInfo.h"
#include "FunctionOverrides.h"
#include "IsoCellSetInlines.h"
#include "JSThreadsSafepoint.h"
#include "Parser.h"
#include "SourceProfiler.h"
#include "Structure.h"
#include "UnlinkedFunctionCodeBlock.h"
#include "VMTraps.h"
#include <wtf/RecursiveLockAdapter.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/Threading.h>

namespace JSC {

WTF_MAKE_TZONE_ALLOCATED_IMPL(UnlinkedFunctionExecutable::ClassElementDefinition);
WTF_MAKE_TZONE_ALLOCATED_IMPL(UnlinkedFunctionExecutable::RareData);

static_assert(sizeof(UnlinkedFunctionExecutable) <= 128, "UnlinkedFunctionExecutable should fit in a 128-byte cell to keep allocated blocks count to only one after initializing JSGlobalObject.");

const ClassInfo UnlinkedFunctionExecutable::s_info = { "UnlinkedFunctionExecutable"_s, nullptr, nullptr, nullptr, CREATE_METHOD_TABLE(UnlinkedFunctionExecutable) };

// UNGIL AB18-R1-C (concurrent unlinked codegen): GIL-off, N mutators of the
// shared VM can race unlinkedCodeBlockFor() for the SAME executable:
//  - recordParse() is a non-atomic read-modify-write of this cell's densely
//    packed bitfield words (m_features / m_hasCapturedVariables share machine
//    words with neighbors like m_parameterCount and m_sourceParseMode), so a
//    concurrent RMW or header read can observe clobbered adjacent bits — a
//    garbage parameter/parse-mode cluster then overflows the checked
//    bookkeeping in BytecodeGenerator::initializeNextParameter (rc=134
//    CrashOnOverflow). Even "same value" concurrent recordParse is UB: C++
//    bitfield writes are word-granularity RMW.
//  - m_unlinkedCodeBlockForCall is a union with RefPtr<Decoder> m_decoder;
//    decodeCachedCodeBlocks() moves the decoder OUT of the slot and decodes
//    INTO it, so a concurrent reader of the slot can dereference a Decoder*
//    or half-written pointer as UnlinkedFunctionCodeBlock* (rc=139 SEGV).
//  - plain double-generate / lost-install on the WriteBarrier slots.
//
// Serialization: REUSE the IT-8 process-wide recursive compilation lock
// (defined in runtime/ScriptExecutable.cpp) rather than introducing a second
// lock. The dominant path already holds it here (prepareForExecutionImpl ->
// newCodeBlockFor -> unlinkedCodeBlockFor), and BytecodeGenerator/DFGPlan take
// it under codegen — any second lock taken across the generate sequence would
// be ABBA-prone against it (the CodeCache eager-generation path enters WITHOUT
// the IT-8 lock held). One recursive lock has no ordering to violate; the
// already-held path is a cheap re-acquire. Per-executable striping is a
// post-acceptance optimization, same as the IT-8 note. Zero per-cell footprint
// (the cell must stay <= 128 bytes, see static_assert above) and no header
// change. GIL-on/flag-off never reaches the lock (one predicted-untaken
// branch), so flag-off identity (V5a) is preserved.
//
// (Resolved residual, AB18-R1-C amendment A: setSingletonHasBeenInvalidated()
// used to set a bit in the same contiguous bitfield run as the recordParse()
// fields from any thread without this lock. The bit has since been hoisted
// out of the bitfield into its own std::atomic<bool>, accessed relaxed via
// the singletonHasBeenInvalidated() accessors — see the member comment in
// UnlinkedFunctionExecutable.h.)
//
// Declared locally (not in a header) — keep in sync with the definition in
// runtime/ScriptExecutable.cpp and the declarations in
// bytecompiler/BytecodeGenerator.cpp, dfg/DFGPlan.cpp and runtime/CodeCache.cpp.
RecursiveLock& gilOffCompilationLock();

namespace {

// Stop-protocol-safe acquisition — same shape as the locker in
// runtime/ScriptExecutable.cpp (see the rationale there): contended
// acquisition spins on tryLock() servicing only NeedStopTheWorld, so a
// waiting lite stays visible to the GIL-off stop fan, never parks raw while
// holding heap access (no watchdogAssertStopProgress regression), and never
// throws. The FIX-2 park poll first — deferral-immune §A.3 parking (callers
// may spin here inside their own DeferTraps scope, under which handleTraps
// correctly no-ops); see the ScriptExecutable.cpp locker comment.
class GILOffCompilationLocker {
    WTF_MAKE_NONCOPYABLE(GILOffCompilationLocker);
public:
    GILOffCompilationLocker(VM& vm, bool shouldLock)
        : m_shouldLock(shouldLock)
    {
        if (!m_shouldLock) [[likely]]
            return;
        RecursiveLock& lock = gilOffCompilationLock();
        if (lock.tryLock()) [[likely]]
            return;
        while (!lock.tryLock()) {
            if (JSThreadsSafepoint::parkSitePollAndParkForStopTheWorld(vm))
                continue; // Parked across a §A.3 window: re-validate (retry tryLock).
            handleTrapsForCurrentThreadIfNeeded(vm, VMTraps::NeedStopTheWorld);
            Thread::yield();
        }
    }

    ~GILOffCompilationLocker()
    {
        if (m_shouldLock) [[unlikely]]
            gilOffCompilationLock().unlock();
    }

private:
    bool m_shouldLock;
};

} // anonymous namespace

static UnlinkedFunctionCodeBlock* generateUnlinkedFunctionCodeBlock(
    VM& vm, UnlinkedFunctionExecutable* executable, const SourceCode& source,
    CodeSpecializationKind kind, OptionSet<CodeGenerationMode> codeGenerationMode,
    UnlinkedFunctionKind functionKind, ParserError& error, SourceParseMode parseMode)
{
    JSParserBuiltinMode builtinMode = executable->isBuiltinFunction() ? JSParserBuiltinMode::Builtin : JSParserBuiltinMode::NotBuiltin;
    JSParserScriptMode scriptMode = executable->scriptMode();
    ASSERT(isFunctionParseMode(executable->parseMode()));
    auto* classElementDefinitions = executable->classElementDefinitions();
    std::unique_ptr<FunctionNode> function = parse<FunctionNode>(
        vm, source, executable->name(), executable->implementationVisibility(), builtinMode, executable->lexicallyScopedFeatures(), scriptMode, executable->parseMode(), executable->functionMode(), executable->superBinding(), error, executable->constructorKind(), executable->derivedContextType(), EvalContextType::None, nullptr, classElementDefinitions);

    if (!function) {
        ASSERT(error.isValid());
        return nullptr;
    }

    function->finishParsing(executable->name(), executable->functionMode());
    executable->recordParse(function->features(), function->lexicallyScopedFeatures(), function->hasCapturedVariables());

    bool isClassContext = executable->superBinding() == SuperBinding::Needed || executable->parseMode() == SourceParseMode::ClassFieldInitializerMode;

    UnlinkedFunctionCodeBlock* result = UnlinkedFunctionCodeBlock::create(vm, FunctionCode, ExecutableInfo(kind == CodeSpecializationKind::CodeForConstruct, executable->privateBrandRequirement(), functionKind == UnlinkedBuiltinFunction, executable->constructorKind(), scriptMode, executable->superBinding(), parseMode, executable->derivedContextType(), executable->needsClassFieldInitializer(), false, isClassContext, executable->evalContextType(), executable->isBuiltinDefaultClassConstructor()), codeGenerationMode);

    auto parentScopeTDZVariables = executable->parentScopeTDZVariables();
    const FixedVector<Identifier>* generatorOrAsyncWrapperFunctionParameterNames = executable->generatorOrAsyncWrapperFunctionParameterNames();
    const PrivateNameEnvironment* parentPrivateNameEnvironment = executable->parentPrivateNameEnvironment();
    error = BytecodeGenerator::generate(vm, function.get(), source, result, codeGenerationMode, parentScopeTDZVariables, generatorOrAsyncWrapperFunctionParameterNames, parentPrivateNameEnvironment);

    if (error.isValid())
        return nullptr;
    vm.codeCache()->updateCache(executable, source, kind, result);
    return result;
}

UnlinkedFunctionExecutable::UnlinkedFunctionExecutable(VM& vm, Structure* structure, const SourceCode& parentSource, FunctionMetadataNode* node, UnlinkedFunctionKind kind, ConstructAbility constructAbility, InlineAttribute inlineAttribute, JSParserScriptMode scriptMode, RefPtr<TDZEnvironmentLink> parentScopeTDZVariables, std::optional<Vector<Identifier>>&& generatorOrAsyncWrapperFunctionParameterNames, std::optional<PrivateNameEnvironment> parentPrivateNameEnvironment, DerivedContextType derivedContextType, EvalContextType evalContextType, NeedsClassFieldInitializer needsClassFieldInitializer, PrivateBrandRequirement privateBrandRequirement, bool isBuiltinDefaultClassConstructor)
    : Base(vm, structure)
    , m_firstLineOffset(node->firstLine() - parentSource.firstLine().oneBasedInt())
    , m_isGeneratedFromCache(false)
    , m_lineCount(node->lastLine() - node->firstLine())
    , m_hasCapturedVariables(false)
    , m_unlinkedFunctionStart(node->functionStart())
    , m_isBuiltinFunction(kind == UnlinkedBuiltinFunction)
    , m_unlinkedBodyStartColumn(node->startColumn())
    , m_isBuiltinDefaultClassConstructor(isBuiltinDefaultClassConstructor)
    , m_unlinkedBodyEndColumn(m_lineCount ? node->endColumn() : node->endColumn() - node->startColumn())
    , m_constructAbility(static_cast<unsigned>(constructAbility))
    , m_startOffset(node->source().startOffset() - parentSource.startOffset())
    , m_scriptMode(static_cast<unsigned>(scriptMode))
    , m_sourceLength(node->source().length())
    , m_superBinding(static_cast<unsigned>(node->superBinding()))
    , m_parametersStartOffset(node->parametersStart())
    , m_isCached(false)
    , m_unlinkedFunctionEnd(node->startStartOffset() + node->source().length() - 1)
    , m_needsClassFieldInitializer(static_cast<unsigned>(needsClassFieldInitializer))
    , m_parameterCount(node->parameterCount())
    , m_privateBrandRequirement(static_cast<unsigned>(privateBrandRequirement))
    , m_features(0)
    , m_constructorKind(static_cast<unsigned>(node->constructorKind()))
    , m_sourceParseMode(node->parseMode())
    , m_implementationVisibility(static_cast<unsigned>(node->implementationVisibility()))
    , m_lexicallyScopedFeatures(node->lexicallyScopedFeatures())
    , m_functionMode(static_cast<unsigned>(node->functionMode()))
    , m_derivedContextType(static_cast<unsigned>(derivedContextType))
    , m_inlineAttribute(static_cast<unsigned>(inlineAttribute))
    , m_evalContextType(static_cast<unsigned>(evalContextType))
    , m_unlinkedCodeBlockForCall()
    , m_unlinkedCodeBlockForConstruct()
    , m_name(node->ident())
    , m_ecmaName(node->ecmaName())
{
    // Make sure these bitfields are adequately wide.
    ASSERT(m_implementationVisibility == static_cast<unsigned>(node->implementationVisibility()));
    ASSERT(m_constructAbility == static_cast<unsigned>(constructAbility));
    ASSERT(m_constructorKind == static_cast<unsigned>(node->constructorKind()));
    ASSERT(m_functionMode == static_cast<unsigned>(node->functionMode()));
    ASSERT(m_scriptMode == static_cast<unsigned>(scriptMode));
    ASSERT(m_superBinding == static_cast<unsigned>(node->superBinding()));
    ASSERT(m_derivedContextType == static_cast<unsigned>(derivedContextType));
    ASSERT(m_inlineAttribute == static_cast<unsigned>(inlineAttribute));
    ASSERT(m_evalContextType == static_cast<unsigned>(evalContextType));
    ASSERT(m_privateBrandRequirement == static_cast<unsigned>(privateBrandRequirement));
    ASSERT(!(m_isBuiltinDefaultClassConstructor && constructorKind() == ConstructorKind::None));
    ASSERT(!m_needsClassFieldInitializer || (isClassConstructorFunction() || derivedContextType == DerivedContextType::DerivedConstructorContext));
    if (!node->classSource().isNull())
        setClassSource(node->classSource());
    if (parentScopeTDZVariables)
        ensureRareData().m_parentScopeTDZVariables = WTF::move(parentScopeTDZVariables);
    if (generatorOrAsyncWrapperFunctionParameterNames)
        ensureRareData().m_generatorOrAsyncWrapperFunctionParameterNames = FixedVector<Identifier>(WTF::move(generatorOrAsyncWrapperFunctionParameterNames.value()));
    if (parentPrivateNameEnvironment)
        ensureRareData().m_parentPrivateNameEnvironment = WTF::move(*parentPrivateNameEnvironment);
}

UnlinkedFunctionExecutable::~UnlinkedFunctionExecutable()
{
    if (m_isCached)
        m_decoder.~RefPtr();
}

void UnlinkedFunctionExecutable::destroy(JSCell* cell)
{
    static_cast<UnlinkedFunctionExecutable*>(cell)->~UnlinkedFunctionExecutable();
}

template<typename Visitor>
void UnlinkedFunctionExecutable::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    UnlinkedFunctionExecutable* thisObject = uncheckedDowncast<UnlinkedFunctionExecutable>(cell);
    ASSERT_GC_OBJECT_INHERITS(thisObject, info());
    Base::visitChildren(thisObject, visitor);

    if (thisObject->codeBlockEdgeMayBeWeak()) {
        auto markIfProfitable = [&] (WriteBarrier<UnlinkedFunctionCodeBlock>& unlinkedCodeBlock) {
            if (!unlinkedCodeBlock)
                return;
            if (unlinkedCodeBlock->didOptimize() == TriState::True)
                visitor.append(unlinkedCodeBlock);
            else if (unlinkedCodeBlock->age() < UnlinkedCodeBlock::maxAge)
                visitor.append(unlinkedCodeBlock);
        };
        markIfProfitable(thisObject->m_unlinkedCodeBlockForCall);
        markIfProfitable(thisObject->m_unlinkedCodeBlockForConstruct);
    } else if (!thisObject->m_isCached) {
        visitor.append(thisObject->m_unlinkedCodeBlockForCall);
        visitor.append(thisObject->m_unlinkedCodeBlockForConstruct);
    }
}

DEFINE_VISIT_CHILDREN(UnlinkedFunctionExecutable);

SourceCode UnlinkedFunctionExecutable::linkedSourceCode(const SourceCode& passedParentSource) const
{
    const SourceCode& parentSource = !m_isBuiltinDefaultClassConstructor ? passedParentSource : BuiltinExecutables::defaultConstructorSourceCode(constructorKind());
    unsigned startColumn = linkedStartColumn(parentSource.startColumn().oneBasedInt());
    unsigned startOffset = parentSource.startOffset() + m_startOffset;
    unsigned firstLine = parentSource.firstLine().oneBasedInt() + m_firstLineOffset;
    return SourceCode(parentSource.provider(), startOffset, startOffset + m_sourceLength, firstLine, startColumn);
}

FunctionExecutable* UnlinkedFunctionExecutable::link(VM& vm, ScriptExecutable* topLevelExecutable, const SourceCode& passedParentSource, std::optional<int> overrideLineNumber, Intrinsic intrinsic, bool isInsideOrdinaryFunction)
{
    SourceCode source = linkedSourceCode(passedParentSource);
    FunctionOverrides::OverrideInfo overrideInfo;
    bool hasFunctionOverride = false;
    if (Options::functionOverrides()) [[unlikely]]
        hasFunctionOverride = FunctionOverrides::initializeOverrideFor(source, overrideInfo);

    if (SourceProfiler::g_profilerHook) [[unlikely]]
        SourceProfiler::profile(SourceProfiler::Type::Function, source);

    FunctionExecutable* result = FunctionExecutable::create(vm, topLevelExecutable, source, this, intrinsic, isInsideOrdinaryFunction);
    if (singletonHasBeenInvalidated())
        result->singleton().invalidate(vm, StringFireDetail("Singleton was previously invalidated"));
    if (overrideLineNumber)
        result->setOverrideLineNumber(*overrideLineNumber);

    if (hasFunctionOverride) [[unlikely]]
        result->overrideInfo(overrideInfo);

    return result;
}

UnlinkedFunctionExecutable* UnlinkedFunctionExecutable::fromGlobalCode(
    const Identifier& name, JSGlobalObject* globalObject, const SourceCode& source, LexicallyScopedFeatures lexicallyScopedFeatures,
    JSObject*& exception, int overrideLineNumber, std::optional<int> functionConstructorParametersEndPosition)
{
    ParserError error;
    VM& vm = globalObject->vm();
    CodeCache* codeCache = vm.codeCache();
    OptionSet<CodeGenerationMode> codeGenerationMode = globalObject->defaultCodeGenerationMode();
    UnlinkedFunctionExecutable* executable = codeCache->getUnlinkedGlobalFunctionExecutable(vm, name, source, lexicallyScopedFeatures, codeGenerationMode, functionConstructorParametersEndPosition, error);

    if (globalObject->hasDebugger())
        globalObject->debugger()->sourceParsed(globalObject, source.provider(), error.line(), error.message());

    if (error.isValid()) {
        exception = error.toErrorObject(globalObject, source, overrideLineNumber);
        return nullptr;
    }

    return executable;
}

UnlinkedFunctionCodeBlock* UnlinkedFunctionExecutable::unlinkedCodeBlockFor(
    VM& vm, const SourceCode& source, CodeSpecializationKind specializationKind, 
    OptionSet<CodeGenerationMode> codeGenerationMode, ParserError& error, SourceParseMode parseMode)
{
    // UNGIL AB18-R1-C mode split: GIL-off, hold the compilation lock across the
    // ENTIRE decode/check/generate/install sequence — not just the install. The
    // crash is inside generation (recordParse bitfield RMW on this cell; the
    // m_decoder/m_unlinkedCodeBlockForCall union flip in decodeCachedCodeBlocks),
    // so a CAS-install alone cannot fix it. The existing cache re-check below
    // doubles as the double-checked load: a losing thread acquires after the
    // winner's unlock, observes the winner's installed block in the switch, and
    // returns it — loser-discard falls out for free. Recursive lock: the
    // prepareForExecutionImpl -> newCodeBlockFor path already holds it; the
    // CodeCache eager-generation recursion re-enters it cheaply. See the
    // rationale block above generateUnlinkedFunctionCodeBlock.
    GILOffCompilationLocker compilationLocker(vm, vm.gilOffWithProcessGate());

    if (m_isCached)
        decodeCachedCodeBlocks(vm);
    switch (specializationKind) {
    case CodeSpecializationKind::CodeForCall:
        if (UnlinkedFunctionCodeBlock* codeBlock = m_unlinkedCodeBlockForCall.get())
            return codeBlock;
        break;
    case CodeSpecializationKind::CodeForConstruct:
        if (UnlinkedFunctionCodeBlock* codeBlock = m_unlinkedCodeBlockForConstruct.get())
            return codeBlock;
        break;
    }

    UnlinkedFunctionCodeBlock* result = generateUnlinkedFunctionCodeBlock(
        vm, this, source, specializationKind, codeGenerationMode, 
        isBuiltinFunction() ? UnlinkedBuiltinFunction : UnlinkedNormalFunction, 
        error, parseMode);
    
    if (error.isValid())
        return nullptr;

    switch (specializationKind) {
    case CodeSpecializationKind::CodeForCall:
        m_unlinkedCodeBlockForCall.set(vm, this, result);
        break;
    case CodeSpecializationKind::CodeForConstruct:
        m_unlinkedCodeBlockForConstruct.set(vm, this, result);
        break;
    }
    // FIXME GlobalGC: Need syncrhonization here for accessing the Heap server.
    vm.heap.unlinkedFunctionExecutableSpaceAndSet.set.add(this);
    return result;
}

void UnlinkedFunctionExecutable::decodeCachedCodeBlocks(VM& vm)
{
    ASSERT(m_isCached);
    ASSERT(m_decoder);
    ASSERT(m_cachedCodeBlockForCallOffset || m_cachedCodeBlockForConstructOffset);

    RefPtr<Decoder> decoder = WTF::move(m_decoder);
    int32_t cachedCodeBlockForCallOffset = m_cachedCodeBlockForCallOffset;
    int32_t cachedCodeBlockForConstructOffset = m_cachedCodeBlockForConstructOffset;

    DeferGC deferGC(vm);

    // No need to clear m_unlinkedCodeBlockForCall here, since we moved the decoder out of the same slot
    if (cachedCodeBlockForCallOffset)
        decodeFunctionCodeBlock(*decoder, cachedCodeBlockForCallOffset, m_unlinkedCodeBlockForCall, this);
    if (cachedCodeBlockForConstructOffset)
        decodeFunctionCodeBlock(*decoder, cachedCodeBlockForConstructOffset, m_unlinkedCodeBlockForConstruct, this);
    else
        m_unlinkedCodeBlockForConstruct.clear();

    WTF::storeStoreFence();
    m_isCached = false;
    vm.writeBarrier(this);
}

UnlinkedFunctionExecutable::RareData& UnlinkedFunctionExecutable::ensureRareDataSlow()
{
    ASSERT(!m_rareData);
    m_rareData = makeUnique<RareData>();
    return *m_rareData;
}

void UnlinkedFunctionExecutable::finalizeUnconditionally(VM& vm, CollectionScope)
{
    if (codeBlockEdgeMayBeWeak()) {
        bool isCleared = false;
        bool isStillValid = false;
        auto clearIfDead = [&] (WriteBarrier<UnlinkedFunctionCodeBlock>& unlinkedCodeBlock) {
            if (!unlinkedCodeBlock)
                return;
            if (!vm.heap.isMarked(unlinkedCodeBlock.get())) {
                unlinkedCodeBlock.clear();
                isCleared = true;
            } else
                isStillValid = true;
        };
        clearIfDead(m_unlinkedCodeBlockForCall);
        clearIfDead(m_unlinkedCodeBlockForConstruct);
        if (isCleared && !isStillValid) {
            // FIXME GlobalGC: Need syncrhonization here for accessing the Heap server.
            vm.heap.unlinkedFunctionExecutableSpaceAndSet.set.remove(this);
        }
    }
}

} // namespace JSC
