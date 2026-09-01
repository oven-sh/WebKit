/*
 * Copyright (C) 2012-2023 Apple Inc. All rights reserved.
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
#include "CodeCache.h"

#include "BytecodeGenerator.h"
#include "DirectEvalExecutable.h"
#include "IndirectEvalExecutable.h"
#include "JSThreadsSafepoint.h"
#include "ModuleProgramExecutable.h"
#include "ProgramExecutable.h"
#include "VariableEnvironmentInlines.h"
#include "VMTraps.h"
#include <wtf/RecursiveLockAdapter.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/Threading.h>

namespace JSC {

WTF_MAKE_TZONE_ALLOCATED_IMPL(CodeCache);

// UNGIL AB18-R1-C: CodeCacheMap (m_sourceCode) is a plain HashMap on the
// shared VM. GIL-off, concurrent findCacheAndUpdateAge (which can prune ->
// m_map.remove) vs addCache (rehash) from N mutators corrupts the table.
// Serialize with the SAME IT-8 process-wide recursive compilation lock as
// UnlinkedFunctionExecutable::unlinkedCodeBlockFor — deliberately NOT a
// separate cache lock: Bun's eager generateUnlinkedCodeBlockForFunctions
// recursion means getUnlinkedGlobalCodeBlock reaches unlinkedCodeBlockFor
// under whatever this file holds, so a second lock would impose a
// cache-before-codegen order that the ScriptExecutable prepareForExecutionImpl
// path (codegen lock first, no cache lock) could invert via any future
// updateCache change — one recursive lock has no order to violate. Cost is
// fine at bring-up: these paths run once per program/eval/Function() source,
// not per call, and GIL-on/flag-off never reaches the lock (one
// predicted-untaken branch), preserving V5a flag-off identity.
//
// Scope: the lock is held across the WHOLE check-then-act pair —
// findCacheAndUpdateAge (including pruneSlowCase's m_map.remove) through
// addCache — in getUnlinkedGlobalCodeBlock and
// getUnlinkedGlobalFunctionExecutable. Double-generate of global code while a
// sibling holds the lock is impossible (whole-pair hold); the loser observes
// the winner's entry on its own pass.
//
// Deliberately NOT locked:
//  - CodeCache::write(): only called from the jsc shell at teardown
//    (jsc.cpp), single-mutator by construction, and has no VM& for the
//    stop-protocol-safe bracket.
//  - CodeCache::clear() (header-inline): runs via VM::deleteAllCode ->
//    whenIdle, i.e. only when the VM is idle with no mutator in these paths.
//
// Declared locally (not in a header) — keep in sync with the definition in
// runtime/ScriptExecutable.cpp and the declarations in
// bytecompiler/BytecodeGenerator.cpp, dfg/DFGPlan.cpp and
// bytecode/UnlinkedFunctionExecutable.cpp.
RecursiveLock& gilOffCompilationLock();

namespace {

// Stop-protocol-safe acquisition — same shape as the locker in
// runtime/ScriptExecutable.cpp (see the rationale there): contended
// acquisition spins on tryLock() servicing only NeedStopTheWorld, so a
// waiting lite stays visible to the GIL-off stop fan, never parks raw while
// holding heap access (no watchdogAssertStopProgress regression), and never
// throws. The FIX-2 park poll first — deferral-immune §A.3 parking; see the
// ScriptExecutable.cpp locker comment.
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

void CodeCacheMap::pruneSlowCase()
{
    m_minCapacity = std::max(m_size - m_sizeAtLastPrune, static_cast<int64_t>(0));
    m_sizeAtLastPrune = m_size;
    m_timeAtLastPrune = ApproximateTime::now();

    if (m_capacity < m_minCapacity)
        m_capacity = m_minCapacity;

    while (m_size > m_capacity || !canPruneQuickly()) {
        // Not begin(): it walks past every empty bucket in front of the first entry, and evicting that entry each
        // time empties the front of the table, so one eviction costs a walk over a large part of the table.
        // The least recently used of a few random entries instead, so a source that is used again and again stays.
        MapType::iterator it = m_map.random();
        for (unsigned i = 1; i < evictionSampleSize; ++i) {
            MapType::iterator other = m_map.random();
            if (other->value.age < it->value.age)
                it = other;
        }

        writeCodeBlock(it->key, it->value);

        m_size -= it->key.length();
        m_map.remove(it);
    }
}

void CodeCacheMap::removeIfHolds(const SourceCodeKey& key, JSCell* cell)
{
    iterator it = m_map.find(key);
    if (it == m_map.end() || it->value.cell.get() != cell)
        return;
    writeCodeBlock(it->key, it->value);
    remove(it);
}

void CodeCacheMap::removeCodeDecodedFromPersistentPayloads()
{
    m_map.removeIf([&](auto& entry) {
        auto* codeBlock = dynamicDowncast<UnlinkedCodeBlock>(entry.value.cell.get());
        if (!codeBlock || !codeBlock->cachedPayloadIndex())
            return false;
        writeCodeBlock(entry.key, entry.value); // as clear() and removeIfHolds() do for what they drop
        m_size -= entry.key.length();
        return true;
    });
}

static void generateUnlinkedCodeBlockForFunctions(VM& vm, UnlinkedCodeBlock* unlinkedCodeBlock, const SourceCode& parentSource, OptionSet<CodeGenerationMode> codeGenerationMode, ParserError& error, unsigned depth, OptimizeBytecode optimize)
{
    if (!depth)
        return;
    auto generate = [&](UnlinkedFunctionExecutable* unlinkedExecutable) {
        // FIXME: We should also generate CodeBlocks for CodeForConstruct of ordinary functions.
        // https://bugs.webkit.org/show_bug.cgi?id=193823
        CodeSpecializationKind kind = unlinkedExecutable->isClassConstructorFunction() ? CodeSpecializationKind::CodeForConstruct : CodeSpecializationKind::CodeForCall;
        SourceCode source = unlinkedExecutable->linkedSourceCode(parentSource);
        UnlinkedFunctionCodeBlock* unlinkedFunctionCodeBlock = unlinkedExecutable->unlinkedCodeBlockFor(vm, source, kind, codeGenerationMode, error, unlinkedExecutable->parseMode(), optimize);
        if (unlinkedFunctionCodeBlock)
            generateUnlinkedCodeBlockForFunctions(vm, unlinkedFunctionCodeBlock, source, codeGenerationMode, error, depth - 1, optimize);
    };

    for (unsigned i = 0; i < unlinkedCodeBlock->numberOfFunctionDecls(); i++)
        generate(unlinkedCodeBlock->functionDecl(i));
    for (unsigned i = 0; i < unlinkedCodeBlock->numberOfFunctionExprs(); i++)
        generate(unlinkedCodeBlock->functionExpr(i));
}

template<class UnlinkedCodeBlockType, class ExecutableType = ScriptExecutable>
UnlinkedCodeBlockType* generateUnlinkedCodeBlockImpl(VM& vm, const SourceCode& source, LexicallyScopedFeatures lexicallyScopedFeatures, JSParserScriptMode scriptMode, OptionSet<CodeGenerationMode> codeGenerationMode, ParserError& error, EvalContextType evalContextType, DerivedContextType derivedContextType, bool isArrowFunctionContext, const TDZEnvironment* variablesUnderTDZ = nullptr, const PrivateNameEnvironment* privateNameEnvironment = nullptr, ExecutableType* executable = nullptr, OptimizeBytecode optimize = OptimizeBytecode::No)
{
    typedef typename CacheTypes<UnlinkedCodeBlockType>::RootNode RootNode;
    bool isInsideOrdinaryFunction = executable && executable->isInsideOrdinaryFunction();

    std::unique_ptr<RootNode> rootNode = parse<RootNode>(
        vm, source, Identifier(), ImplementationVisibility::Public, JSParserBuiltinMode::NotBuiltin, lexicallyScopedFeatures, scriptMode, CacheTypes<UnlinkedCodeBlockType>::parseMode, FunctionMode::None, SuperBinding::NotNeeded, error, ConstructorKind::None, derivedContextType, evalContextType, privateNameEnvironment, nullptr, isInsideOrdinaryFunction);

    if (!rootNode)
        return nullptr;

    if (executable)
        executable->recordParse(rootNode->features(), rootNode->lexicallyScopedFeatures(), rootNode->hasCapturedVariables());

    NeedsClassFieldInitializer needsClassFieldInitializer = NeedsClassFieldInitializer::No;
    PrivateBrandRequirement privateBrandRequirement = PrivateBrandRequirement::None;
    if constexpr (std::is_same_v<ExecutableType, DirectEvalExecutable>) {
        needsClassFieldInitializer = executable->needsClassFieldInitializer();
        privateBrandRequirement = executable->privateBrandRequirement();
    }
    ExecutableInfo executableInfo(false, privateBrandRequirement, false, ConstructorKind::None, scriptMode, SuperBinding::NotNeeded, CacheTypes<UnlinkedCodeBlockType>::parseMode, derivedContextType, needsClassFieldInitializer, isArrowFunctionContext, false, evalContextType);

    UnlinkedCodeBlockType* unlinkedCodeBlock = UnlinkedCodeBlockType::create(vm, executableInfo, codeGenerationMode);
    unlinkedCodeBlock->recordParse(rootNode->features(), rootNode->lexicallyScopedFeatures(), rootNode->hasCapturedVariables());
    if (!source.provider()->sourceURLDirective().isNull())
        unlinkedCodeBlock->setSourceURLDirective(source.provider()->sourceURLDirective());
    if (!source.provider()->sourceMappingURLDirective().isNull())
        unlinkedCodeBlock->setSourceMappingURLDirective(source.provider()->sourceMappingURLDirective());

    RefPtr<TDZEnvironmentLink> parentVariablesUnderTDZ;
    if (variablesUnderTDZ)
        parentVariablesUnderTDZ = TDZEnvironmentLink::create(vm.m_compactVariableMap->get(*variablesUnderTDZ), nullptr);
    error = BytecodeGenerator::generate(vm, rootNode.get(), source, unlinkedCodeBlock, codeGenerationMode, parentVariablesUnderTDZ, nullptr, privateNameEnvironment, optimize);

    if (error.isValid())
        return nullptr;

    return unlinkedCodeBlock;
}

template<class UnlinkedCodeBlockType, class ExecutableType>
UnlinkedCodeBlockType* generateUnlinkedCodeBlock(VM& vm, ExecutableType* executable, const SourceCode& source, JSParserScriptMode scriptMode, OptionSet<CodeGenerationMode> codeGenerationMode, ParserError& error, EvalContextType evalContextType, const TDZEnvironment* variablesUnderTDZ = nullptr, const PrivateNameEnvironment* privateNameEnvironment = nullptr)
{
    return generateUnlinkedCodeBlockImpl<UnlinkedCodeBlockType, ExecutableType>(vm, source, executable->lexicallyScopedFeatures(), scriptMode, codeGenerationMode, error, evalContextType, executable->derivedContextType(), executable->isArrowFunctionContext(), variablesUnderTDZ, privateNameEnvironment, executable);
}

UnlinkedEvalCodeBlock* generateUnlinkedCodeBlockForDirectEval(VM& vm, DirectEvalExecutable* executable, const SourceCode& source, JSParserScriptMode scriptMode, OptionSet<CodeGenerationMode> codeGenerationMode, ParserError& error, EvalContextType evalContextType, const TDZEnvironment* variablesUnderTDZ, const PrivateNameEnvironment* privateNameEnvironment)
{
    return generateUnlinkedCodeBlock<UnlinkedEvalCodeBlock>(vm, executable, source, scriptMode, codeGenerationMode, error, evalContextType, variablesUnderTDZ, privateNameEnvironment);
}

template <class UnlinkedCodeBlockType>
    requires (!std::same_as<UnlinkedCodeBlockType, UnlinkedEvalCodeBlock>)
UnlinkedCodeBlockType* recursivelyGenerateUnlinkedCodeBlock(VM& vm, const SourceCode& source, LexicallyScopedFeatures lexicallyScopedFeatures, JSParserScriptMode scriptMode, OptionSet<CodeGenerationMode> codeGenerationMode, ParserError& error, EvalContextType evalContextType, unsigned depth, OptimizeBytecode optimize)
{
    bool isArrowFunctionContext = false;
    UnlinkedCodeBlockType* unlinkedCodeBlock = generateUnlinkedCodeBlockImpl<UnlinkedCodeBlockType>(vm, source, lexicallyScopedFeatures, scriptMode, codeGenerationMode, error, evalContextType, DerivedContextType::None, isArrowFunctionContext, nullptr, nullptr, static_cast<ScriptExecutable*>(nullptr), optimize);
    if (!unlinkedCodeBlock)
        return nullptr;

    generateUnlinkedCodeBlockForFunctions(vm, unlinkedCodeBlock, source, codeGenerationMode, error, depth, optimize);
    return unlinkedCodeBlock;
}

void recursivelyGenerateUnlinkedCodeBlocksForFunction(VM& vm, UnlinkedFunctionExecutable* executable, const SourceCode& parentSource, ParserError& error, unsigned depth, OptimizeBytecode optimize)
{
    SourceCode source = executable->linkedSourceCode(parentSource);
    UnlinkedFunctionCodeBlock* codeBlock = executable->unlinkedCodeBlockFor(vm, source, executable->isClassConstructorFunction() ? CodeSpecializationKind::CodeForConstruct : CodeSpecializationKind::CodeForCall, { }, error, executable->parseMode(), optimize);
    if (codeBlock)
        generateUnlinkedCodeBlockForFunctions(vm, codeBlock, source, { }, error, depth, optimize);
}

UnlinkedProgramCodeBlock* recursivelyGenerateUnlinkedCodeBlockForProgram(VM& vm, const SourceCode& source, LexicallyScopedFeatures lexicallyScopedFeatures, JSParserScriptMode scriptMode, OptionSet<CodeGenerationMode> codeGenerationMode, ParserError& error, EvalContextType evalContextType, unsigned depth, OptimizeBytecode optimize)
{
    return recursivelyGenerateUnlinkedCodeBlock<UnlinkedProgramCodeBlock>(vm, source, lexicallyScopedFeatures, scriptMode, codeGenerationMode, error, evalContextType, depth, optimize);
}

UnlinkedModuleProgramCodeBlock* recursivelyGenerateUnlinkedCodeBlockForModuleProgram(VM& vm, const SourceCode& source, LexicallyScopedFeatures lexicallyScopedFeatures, JSParserScriptMode scriptMode, OptionSet<CodeGenerationMode> codeGenerationMode, ParserError& error, EvalContextType evalContextType, unsigned depth, OptimizeBytecode optimize)
{
    return recursivelyGenerateUnlinkedCodeBlock<UnlinkedModuleProgramCodeBlock>(vm, source, lexicallyScopedFeatures, scriptMode, codeGenerationMode, error, evalContextType, depth, optimize);
}

void recordParseFromUnlinkedCodeBlock(GlobalExecutable* executable, const SourceCode& source, UnlinkedGlobalCodeBlock* unlinkedCodeBlock)
{
    executable->recordParse(unlinkedCodeBlock->codeFeatures(), unlinkedCodeBlock->lexicallyScopedFeatures(), unlinkedCodeBlock->hasCapturedVariables());
    if (unlinkedCodeBlock->sourceURLDirective())
        source.provider()->setSourceURLDirective(unlinkedCodeBlock->sourceURLDirective());
    if (unlinkedCodeBlock->sourceMappingURLDirective())
        source.provider()->setSourceMappingURLDirective(unlinkedCodeBlock->sourceMappingURLDirective());
}

template<class UnlinkedCodeBlockType, class ExecutableType>
UnlinkedCodeBlockType* CodeCache::getUnlinkedGlobalCodeBlock(VM& vm, ExecutableType* executable, const SourceCode& source, JSParserScriptMode scriptMode, OptionSet<CodeGenerationMode> codeGenerationMode, ParserError& error, EvalContextType evalContextType)
{
    // UNGIL AB18-R1-C mode split: GIL-off, hold the compilation lock across the
    // whole findCacheAndUpdateAge (may prune -> m_map.remove) / generate /
    // addCache (may rehash) sequence. The eager
    // generateUnlinkedCodeBlockForFunctions recursion below reaches
    // UnlinkedFunctionExecutable::unlinkedCodeBlockFor, which re-enters this
    // same recursive lock — see the rationale block above pruneSlowCase.
    GILOffCompilationLocker compilationLocker(vm, vm.gilOffWithProcessGate());

    DerivedContextType derivedContextType = executable->derivedContextType();
    bool isArrowFunctionContext = executable->isArrowFunctionContext();
    SourceCodeKey key(
        source, String(), CacheTypes<UnlinkedCodeBlockType>::codeType, executable->lexicallyScopedFeatures(), scriptMode,
        derivedContextType, evalContextType, isArrowFunctionContext, codeGenerationMode,
        std::nullopt);
    // Code of a module whose loader has a module scope of its own resolves that scope's
    // variables as closure variables where other records of the same source resolve
    // globals, so it cannot use the unlinked code those share (the baseline code cached
    // on it assumes one resolution: JIT::emit_op_resolve_scope); it gets its own, which
    // the records that share its executable then use.
    bool privateToExecutable = false;
    if constexpr (std::is_same_v<ExecutableType, ModuleProgramExecutable>)
        privateToExecutable = !executable->resolvesInGlobalScope();
    // (Nor is it registered for being dropped and decoded again: what is remembered for that is remembered per payload
    // and provider, which the shared code of the same source may have as well.)
    UnlinkedCodeBlockType* unlinkedCodeBlock = privateToExecutable ? m_sourceCode.fetchFromDisk<UnlinkedCodeBlockType>(vm, key, Decoder::RecoverableCode::No) : m_sourceCode.findCacheAndUpdateAge<UnlinkedCodeBlockType>(vm, key);
    if (unlinkedCodeBlock && Options::useCodeCache()) {
        recordParseFromUnlinkedCodeBlock(executable, source, unlinkedCodeBlock);
        return unlinkedCodeBlock;
    }

    unlinkedCodeBlock = generateUnlinkedCodeBlock<UnlinkedCodeBlockType, ExecutableType>(vm, executable, source, scriptMode, codeGenerationMode, error, evalContextType);

    if (unlinkedCodeBlock && Options::useCodeCache() && !privateToExecutable) {
        m_sourceCode.addCache(key, SourceCodeValue(vm, unlinkedCodeBlock, m_sourceCode.age()));

        key.source().provider().cacheBytecode([&] {
            return encodeCodeBlock(vm, key, unlinkedCodeBlock);
        });
#if USE(BUN_JSC_ADDITIONS)
        key.source().provider().didGenerateUnlinkedCodeBlock(vm, key, unlinkedCodeBlock);
#endif
    }

    return unlinkedCodeBlock;
}

UnlinkedProgramCodeBlock* CodeCache::getUnlinkedProgramCodeBlock(VM& vm, ProgramExecutable* executable, const SourceCode& source, OptionSet<CodeGenerationMode> codeGenerationMode, ParserError& error)
{
    return getUnlinkedGlobalCodeBlock<UnlinkedProgramCodeBlock>(vm, executable, source, JSParserScriptMode::Classic, codeGenerationMode, error, EvalContextType::None);
}

UnlinkedEvalCodeBlock* CodeCache::getUnlinkedEvalCodeBlock(VM& vm, IndirectEvalExecutable* executable, const SourceCode& source, OptionSet<CodeGenerationMode> codeGenerationMode, ParserError& error, EvalContextType evalContextType)
{
    return getUnlinkedGlobalCodeBlock<UnlinkedEvalCodeBlock>(vm, executable, source, JSParserScriptMode::Classic, codeGenerationMode, error, evalContextType);
}

UnlinkedModuleProgramCodeBlock* CodeCache::getUnlinkedModuleProgramCodeBlock(VM& vm, ModuleProgramExecutable* executable, const SourceCode& source, OptionSet<CodeGenerationMode> codeGenerationMode, ParserError& error)
{
    return getUnlinkedGlobalCodeBlock<UnlinkedModuleProgramCodeBlock>(vm, executable, source, JSParserScriptMode::Module, codeGenerationMode, error, EvalContextType::None);
}

void CodeCache::forgetUnlinkedModuleProgramCodeBlock(ModuleProgramExecutable* executable, const SourceCode& source, UnlinkedModuleProgramCodeBlock* unlinkedCodeBlock)
{
    SourceCodeKey key(
        source, String(), SourceCodeType::ModuleType, executable->lexicallyScopedFeatures(), JSParserScriptMode::Module,
        executable->derivedContextType(), EvalContextType::None, executable->isArrowFunctionContext(), unlinkedCodeBlock->codeGenerationMode(),
        std::nullopt);
    m_sourceCode.removeIfHolds(key, unlinkedCodeBlock);
}

UnlinkedFunctionExecutable* CodeCache::getUnlinkedGlobalFunctionExecutable(VM& vm, const Identifier& name, const SourceCode& source, LexicallyScopedFeatures lexicallyScopedFeatures, OptionSet<CodeGenerationMode> codeGenerationMode, std::optional<int> functionConstructorParametersEndPosition, ParserError& error)
{
    // UNGIL AB18-R1-C mode split: same whole-pair hold as
    // getUnlinkedGlobalCodeBlock — findCacheAndUpdateAge through addCache, plus
    // the recordParse() bitfield RMW on the freshly created executable, all
    // under the one recursive compilation lock (see above pruneSlowCase).
    GILOffCompilationLocker compilationLocker(vm, vm.gilOffWithProcessGate());

    bool isArrowFunctionContext = false;
    SourceCodeKey key(
        source, name.string(), SourceCodeType::FunctionType,
        lexicallyScopedFeatures,
        JSParserScriptMode::Classic,
        DerivedContextType::None,
        EvalContextType::FunctionEvalContext,
        isArrowFunctionContext,
        codeGenerationMode,
        functionConstructorParametersEndPosition);
    UnlinkedFunctionExecutable* executable = m_sourceCode.findCacheAndUpdateAge<UnlinkedFunctionExecutable>(vm, key);
    if (executable && Options::useCodeCache()) {
        if (!executable->sourceURLDirective().isNull())
            source.provider()->setSourceURLDirective(executable->sourceURLDirective());
        if (!executable->sourceMappingURLDirective().isNull())
            source.provider()->setSourceMappingURLDirective(executable->sourceMappingURLDirective());
        return executable;
    }

    std::unique_ptr<ProgramNode> program = parseFunctionForFunctionConstructor(vm, source, lexicallyScopedFeatures, error, functionConstructorParametersEndPosition);
    if (!program) {
        RELEASE_ASSERT(error.isValid());
        return nullptr;
    }

    // This function assumes an input string that would result in a single function declaration.
    StatementNode* funcDecl = program->singleStatement();
    if (!funcDecl) [[unlikely]] {
        JSToken token;
        error = ParserError(ParserError::SyntaxError, ParserError::SyntaxErrorIrrecoverable, token, "Parser error"_s, -1);
        return nullptr;
    }
    ASSERT(funcDecl->isFuncDeclNode());

    FunctionMetadataNode* metadata = static_cast<FuncDeclNode*>(funcDecl)->metadata();
    ASSERT(metadata);
    if (!metadata)
        return nullptr;

    metadata->overrideName(name);
    // The Function constructor only has access to global variables, so no variables will be under TDZ unless they're
    // in the global lexical environment, which we always TDZ check accesses from.
    ConstructAbility constructAbility = constructAbilityForParseMode(metadata->parseMode());
    UnlinkedFunctionExecutable* functionExecutable = UnlinkedFunctionExecutable::create(vm, source, metadata, UnlinkedNormalFunction, constructAbility, InlineAttribute::None, JSParserScriptMode::Classic, nullptr, { }, std::nullopt, DerivedContextType::None, EvalContextType::FunctionEvalContext, NeedsClassFieldInitializer::No, PrivateBrandRequirement::None);

    if (!source.provider()->sourceURLDirective().isNull())
        functionExecutable->setSourceURLDirective(source.provider()->sourceURLDirective());
    if (!source.provider()->sourceMappingURLDirective().isNull())
        functionExecutable->setSourceMappingURLDirective(source.provider()->sourceMappingURLDirective());

    // We initially start with hasCapturedVariables = false.
    functionExecutable->recordParse(program->features(), metadata->lexicallyScopedFeatures(), /* hasCapturedVariables */ false);

    if (Options::useCodeCache())
        m_sourceCode.addCache(key, SourceCodeValue(vm, functionExecutable, m_sourceCode.age()));
    return functionExecutable;
}

void CodeCache::updateCache(const UnlinkedFunctionExecutable* executable, const SourceCode& parentSource, CodeSpecializationKind kind, const UnlinkedFunctionCodeBlock* codeBlock)
{
    parentSource.provider()->updateCache(executable, parentSource, kind, codeBlock);
}

void CodeCache::write()
{
    // UNGIL AB18-R1-C: deliberately unlocked — jsc-shell teardown only,
    // single-mutator by construction, and no VM& for the stop-protocol-safe
    // bracket. See the rationale block above pruneSlowCase.
    for (auto& it : m_sourceCode)
        writeCodeBlock(it.key, it.value);
}

void writeCodeBlock(const SourceCodeKey& key, const SourceCodeValue& value)
{
    UnlinkedCodeBlock* codeBlock = dynamicDowncast<UnlinkedCodeBlock>(value.cell.get());
    if (!codeBlock)
        return;

    key.source().provider().commitCachedBytecode();
}

static SourceCodeKey sourceCodeKeyForSerializedBytecode(VM&, const SourceCode& sourceCode, SourceCodeType codeType, LexicallyScopedFeatures lexicallyScopedFeatures, JSParserScriptMode scriptMode, OptionSet<CodeGenerationMode> codeGenerationMode)
{
    return SourceCodeKey(
        sourceCode, String(), codeType, lexicallyScopedFeatures, scriptMode,
        DerivedContextType::None, EvalContextType::None, false, codeGenerationMode,
        std::nullopt);
}

SourceCodeKey sourceCodeKeyForSerializedProgram(VM& vm, const SourceCode& sourceCode)
{
    JSParserScriptMode scriptMode = JSParserScriptMode::Classic;
    return sourceCodeKeyForSerializedBytecode(vm, sourceCode, SourceCodeType::ProgramType, NoLexicallyScopedFeatures, scriptMode, {});
}

SourceCodeKey sourceCodeKeyForSerializedModule(VM& vm, const SourceCode& sourceCode)
{
    JSParserScriptMode scriptMode = JSParserScriptMode::Module;
    return sourceCodeKeyForSerializedBytecode(vm, sourceCode, SourceCodeType::ModuleType, StrictModeLexicallyScopedFeature, scriptMode, {});
}

RefPtr<CachedBytecode> serializeBytecode(VM& vm, UnlinkedCodeBlock* codeBlock, const SourceCode& source, SourceCodeType codeType, LexicallyScopedFeatures lexicallyScopedFeatures, JSParserScriptMode scriptMode, FileSystem::FileHandle& fileHandle, BytecodeCacheError& error, OptionSet<CodeGenerationMode> codeGenerationMode)
{
    return encodeCodeBlock(vm, sourceCodeKeyForSerializedBytecode(vm, source, codeType, lexicallyScopedFeatures, scriptMode, codeGenerationMode), codeBlock, fileHandle, error);
}

}
