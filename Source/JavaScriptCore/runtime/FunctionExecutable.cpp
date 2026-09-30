/*
 * Copyright (C) 2009-2021 Apple Inc. All rights reserved.
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
#include "FunctionExecutable.h"

#include "CodeBlock.h"
#include "FunctionCodeBlock.h"
#include "FunctionConstructor.h"
#include "FunctionExecutableInlines.h"
#include "FunctionOverrides.h"
#include "SourceCodeKey.h"
#include "IsoCellSetInlines.h"
#include "JSArray.h"
#include "JSCJSValueInlines.h"
#include "StaticHeap.h"
#include <wtf/Threading.h>

namespace JSC {

const ClassInfo FunctionExecutable::s_info = { "FunctionExecutable"_s, &ScriptExecutable::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(FunctionExecutable) };

FunctionExecutable::FunctionExecutable(VM& vm, ScriptExecutable* topLevelExecutable, const SourceCode& source, UnlinkedFunctionExecutable* unlinkedExecutable, Intrinsic intrinsic, bool isInsideOrdinaryFunction)
    : ScriptExecutable(vm.functionExecutableStructure.get(), vm, source, unlinkedExecutable->lexicallyScopedFeatures(), unlinkedExecutable->derivedContextType(), false, isInsideOrdinaryFunction || !unlinkedExecutable->isArrowFunction(), EvalContextType::None, intrinsic)
    , m_topLevelExecutable(topLevelExecutable ? topLevelExecutable : this, WriteBarrierEarlyInit)
    , m_unlinkedExecutable(unlinkedExecutable, WriteBarrierEarlyInit)
{
    RELEASE_ASSERT(!source.isNull());
    ASSERT(source.length());
}

void FunctionExecutable::becomeStatic(VM& vm)
{
    static_assert(OBJECT_OFFSETOF(FunctionExecutable, m_jitCodeForCallWithArityCheck) == sizeOfShortForm);
    // Which realm's it is remains to be seen (topLevelExecutable()), and there is going to be more than one function made of it, or
    // there may as well be.
    m_topLevelExecutable.clear();
    m_singleton.invalidate(vm, StringFireDetail("Made when the program was built"));
}

// AOT::Stub::EnterStaticFunctionForCall and AOT::Stub::EnterStaticFunctionForConstruct, once there is an AOT::RuntimeTable. (The interpreter goes by this as well: virtualThunkFor.)
extern "C" {
JS_EXPORT_PRIVATE void* g_aotWaysIntoStaticFunctions[2] { };
}

CodePtr<JSEntryPtrTag> ExecutableBase::wayIntoShortForm(CodeSpecializationKind kind) const
{
    unsigned which = static_cast<unsigned>(kind);
    if (!m_aotEntry[which])
        return nullptr;
    // (See FunctionExecutable::aotIndexOfWhatConstructsByCalling.)
    if (kind == CodeSpecializationKind::CodeForConstruct && m_aotIndex[which] == FunctionExecutable::aotIndexOfWhatConstructsByCalling)
        return CodePtr<JSEntryPtrTag>::fromTaggedPtr(std::bit_cast<void*>(m_aotEntry[which]));
    ASSERT(g_aotWaysIntoStaticFunctions[which]);
    return CodePtr<JSEntryPtrTag>::fromTaggedPtr(g_aotWaysIntoStaticFunctions[which]);
}

void FunctionExecutable::setAOTCode(CodeSpecializationKind kind, void* stub, uint64_t entry, uint32_t index)
{
    m_aotEntry[static_cast<unsigned>(kind)] = entry;
    m_aotIndex[static_cast<unsigned>(kind)] = index;
    (isCall(kind) ? m_jitCodeForCallWithArityCheck : m_jitCodeForConstructWithArityCheck) = CodePtr<JSEntryPtrTag>::fromTaggedPtr(stub);
}

ScriptExecutable* FunctionExecutable::topLevelExecutableOfStaticExecutable() const
{
    SourceProvider* provider = sourceProvider();
    RELEASE_ASSERT(StaticHeap::isPlaceOfSourceProvider(provider));
    return StaticHeap::topLevelExecutableOfModuleWithProvider(vm(), provider);
}

void FunctionExecutable::destroy(JSCell* cell)
{
    static_cast<FunctionExecutable*>(cell)->FunctionExecutable::~FunctionExecutable();
}

CString FunctionExecutable::inferredNameForTools()
{
    // Only the thread running the VM may pull the name out of the bytecode cache (it atomizes); compiler, GC, sampling
    // profiler and crash-reporter threads print what is there.
    if (isCompilationThread() || Thread::mayBeGCThread() || !vm().currentThreadIsHoldingAPILock()) {
        if (const Identifier* name = tryGetEcmaNameConcurrently())
            return name->utf8();
        return "<name not materialized>"_span;
    }
    // The mutator itself may be inside the collector's end phase (a CodeBlock dumped while it is jettisoned), where it must not atomize either.
    return ecmaNameWithoutGC().utf8();
}

FunctionCodeBlock* FunctionExecutable::baselineCodeBlockFor(CodeSpecializationKind kind)
{
    CodeBlock* codeBlock = nullptr;
    if (kind == CodeSpecializationKind::CodeForCall)
        codeBlock = codeBlockForCall();
    else {
        RELEASE_ASSERT(kind == CodeSpecializationKind::CodeForConstruct);
        codeBlock = codeBlockForConstruct();
    }
    if (!codeBlock)
        return nullptr;
    return static_cast<FunctionCodeBlock*>(codeBlock->baselineAlternative());
}

template<typename Visitor>
static inline bool shouldKeepInConstraintSet(Visitor& visitor, CodeBlock* codeBlockForCall, CodeBlock* codeBlockForConstruct)
{
    // If either CodeBlock is not marked yet, we will run output-constraints.
    return (codeBlockForCall && !visitor.isMarked(codeBlockForCall)) || (codeBlockForConstruct && !visitor.isMarked(codeBlockForConstruct));
}

template<typename Visitor>
void FunctionExecutable::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    VM& vm = visitor.vm();
    FunctionExecutable* thisObject = uncheckedDowncast<FunctionExecutable>(cell);
    ASSERT_GC_OBJECT_INHERITS(thisObject, info());
    Base::visitChildren(thisObject, visitor);
    if (thisObject->isShortForm())
        return;
    thisObject = thisObject->inFull();
#if USE(BUN_JSC_ADDITIONS)
    thisObject->visitSourceFetcher(visitor);
#endif
    visitor.append(thisObject->m_topLevelExecutable);
    visitor.append(thisObject->m_unlinkedExecutable);
    if (RareData* rareData = thisObject->m_rareData.get()) {
        visitor.append(rareData->m_cachedPolyProtoStructureID);
        visitor.append(rareData->m_asString);
        if (TemplateObjectMap* map = rareData->m_templateObjectMap.get()) {
            Locker locker { thisObject->cellLock() };
            for (auto& entry : *map)
                visitor.append(entry.value);
        }
    }

    // Every live FunctionExecutable needs reconciling, so it is not tracked via weakReconciliationSet.
    auto* codeBlockForCall = thisObject->m_codeBlockForCall.get();
    if (codeBlockForCall)
        visitCodeBlockEdge(visitor, codeBlockForCall);
    auto* codeBlockForConstruct = thisObject->m_codeBlockForConstruct.get();
    if (codeBlockForConstruct)
        visitCodeBlockEdge(visitor, codeBlockForConstruct);

    // (One that was made when the program was built is in no set of cells of the collector's. It is looked at every time.)
    if (StaticHeap::contains(thisObject)) [[unlikely]] {
        visitor.append(thisObject->m_codeBlockForCall);
        visitor.append(thisObject->m_codeBlockForConstruct);
        return;
    }

    if (shouldKeepInConstraintSet(visitor, codeBlockForCall, codeBlockForConstruct))
        vm.heap.functionExecutableSpaceAndSet.outputConstraintsSet.add(thisObject);
}

DEFINE_VISIT_CHILDREN(FunctionExecutable);

template<typename Visitor>
void FunctionExecutable::visitOutputConstraintsImpl(JSCell* cell, Visitor& visitor)
{
    VM& vm = visitor.vm();
    auto* executable = uncheckedDowncast<FunctionExecutable>(cell);
    auto* codeBlockForCall = executable->m_codeBlockForCall.get();
    if (codeBlockForCall) {
        if (!visitor.isMarked(codeBlockForCall))
            runConstraint(NoLockingNecessary, visitor, codeBlockForCall);
    }
    auto* codeBlockForConstruct = executable->codeBlockForConstruct();
    if (codeBlockForConstruct) {
        if (!visitor.isMarked(codeBlockForConstruct))
            runConstraint(NoLockingNecessary, visitor, codeBlockForConstruct);
    }

    if (!shouldKeepInConstraintSet(visitor, codeBlockForCall, codeBlockForConstruct))
        vm.heap.functionExecutableSpaceAndSet.outputConstraintsSet.remove(executable);
}

DEFINE_VISIT_OUTPUT_CONSTRAINTS(FunctionExecutable);

FunctionExecutable* FunctionExecutable::fromGlobalCode(const Identifier& name, JSGlobalObject* globalObject, String&& program, const SourceOrigin& sourceOrigin, SourceTaintedOrigin taintedOrigin, const String& sourceURL, const TextPosition& position, LexicallyScopedFeatures lexicallyScopedFeatures, JSObject*& exception, int overrideLineNumber, std::optional<int> functionConstructorParametersEndPosition, FunctionConstructionMode functionConstructionMode)
{
    if (overrideLineNumber == overrideLineNumberNotFound) {
        if (auto* executable = globalObject->tryGetCachedFunctionExecutableForFunctionConstructor(name, program, sourceOrigin, taintedOrigin, sourceURL, position, lexicallyScopedFeatures, functionConstructionMode))
            return executable;
    }

    auto source = makeSource(WTF::move(program), sourceOrigin, taintedOrigin, sourceURL, position);
    UnlinkedFunctionExecutable* unlinkedExecutable = 
        UnlinkedFunctionExecutable::fromGlobalCode(
            name, globalObject, source, lexicallyScopedFeatures, exception, overrideLineNumber, functionConstructorParametersEndPosition);
    if (!unlinkedExecutable)
        return nullptr;

    auto* executable = unlinkedExecutable->link(globalObject->vm(), nullptr, source, overrideLineNumber);
    if (overrideLineNumber == overrideLineNumberNotFound) {
        if (executable)
            globalObject->cachedFunctionExecutableForFunctionConstructor(executable);
    }
    return executable;
}

// ---- The short form

SourceProvider* ScriptExecutable::sourceProviderOfShortForm() const
{
    return StaticHeap::sourceProviderOfModule(StaticHeap::rowOf(indexOfShortForm()).module);
}

LineColumn ScriptExecutable::whereShortFormStarts() const
{
    return StaticHeap::whereFunctionStarts(indexOfShortForm());
}

// Hardly anybody asks, and what they want is which source it is: there is no text for the rest to be about.
const SourceCode& ScriptExecutable::sourceOfShortForm() const
{
    static Lock lock;
    static NeverDestroyed<UncheckedKeyHashMap<uint32_t, std::unique_ptr<SourceCode>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>> sources;
    Locker locker { lock };
    return *sources->ensure(indexOfShortForm(), [&] {
        LineColumn start = whereShortFormStarts();
        return makeUniqueWithoutFastMallocCheck<SourceCode>(RefPtr { sourceProviderOfShortForm() }, 0, 0, static_cast<int>(start.line), static_cast<int>(start.column));
    }).iterator->value;
}

CodeFeatures ScriptExecutable::featuresOfShortForm() const
{
    return uncheckedDowncast<FunctionExecutable>(this)->unlinkedExecutable()->features();
}

LexicallyScopedFeatures ScriptExecutable::lexicallyScopedFeaturesOfShortForm() const
{
    return uncheckedDowncast<FunctionExecutable>(this)->unlinkedExecutable()->lexicallyScopedFeatures();
}

DerivedContextType ScriptExecutable::derivedContextTypeOfShortForm() const
{
    return uncheckedDowncast<FunctionExecutable>(this)->unlinkedExecutable()->derivedContextType();
}

FunctionExecutable::RareData& FunctionExecutable::ensureRareDataSlow()
{
    ASSERT(!m_rareData);
    auto rareData = makeUnique<RareData>();
    rareData->m_lineCount = lineCount();
    rareData->m_endColumn = endColumn();
    rareData->m_parametersStartOffset = parametersStartOffset();
    rareData->m_functionStart = functionStart();
    rareData->m_functionEnd = functionEnd();
    WTF::storeStoreFence();
    m_rareData = WTF::move(rareData);
    return *m_rareData;
}

JSString* FunctionExecutable::toStringSlow(JSGlobalObject* globalObject)
{
    VM& vm = getVM(globalObject);
    ASSERT(!rareData() || !rareData()->m_asString);

    auto throwScope = DECLARE_THROW_SCOPE(vm);

    const auto& cache = [&](JSString* asString) {
        if (!rareData())
            return asString;
        WTF::storeStoreFence();
        rareData()->m_asString.set(vm, this, asString);
        return asString;
    };

    const auto& cacheIfNoException = [&](JSValue value) -> JSString* {
        RETURN_IF_EXCEPTION(throwScope, nullptr);
        return cache(::JSC::asString(value));
    };

#if USE(BUN_JSC_ADDITIONS)
    if (isPrivateBuiltinFunction())
#else
    if (isBuiltinFunction())
#endif
        return cacheIfNoException(jsMakeNontrivialString(globalObject, "function "_s, name().string(), "() { [native code] }"_s));

#if USE(BUN_JSC_ADDITIONS)
    // It still starts the way whoever tells one kind of function from another by its text expects.
    // (The source of a constructor that nobody wrote is one of the engine's own. Its class's is the program's.)
    if ((isClass() ? classSource().provider() : sourceProvider())->hasNoText()) {
        if (isClass())
            return cacheIfNoException(jsMakeNontrivialString(globalObject, "class "_s, ecmaName().string(), " { [native code] }"_s));
        ASCIILiteral before = "function "_s;
        bool isNamed = true;
        switch (parseMode()) {
        case SourceParseMode::ArrowFunctionMode:
            before = ""_s;
            isNamed = false;
            break;
        case SourceParseMode::AsyncArrowFunctionMode:
        case SourceParseMode::AsyncArrowFunctionBodyMode:
            before = "async "_s;
            isNamed = false;
            break;
        case SourceParseMode::GeneratorWrapperFunctionMode:
        case SourceParseMode::GeneratorBodyMode:
            before = "function* "_s;
            break;
        case SourceParseMode::AsyncFunctionMode:
        case SourceParseMode::AsyncFunctionBodyMode:
            before = "async function "_s;
            break;
        case SourceParseMode::AsyncGeneratorWrapperFunctionMode:
        case SourceParseMode::AsyncGeneratorBodyMode:
            before = "async function* "_s;
            break;
        case SourceParseMode::MethodMode:
            before = ""_s;
            break;
        case SourceParseMode::GeneratorWrapperMethodMode:
            before = "*"_s;
            break;
        case SourceParseMode::AsyncMethodMode:
            before = "async "_s;
            break;
        case SourceParseMode::AsyncGeneratorWrapperMethodMode:
            before = "async *"_s;
            break;
        case SourceParseMode::GetterMode:
            before = "get "_s;
            break;
        case SourceParseMode::SetterMode:
            before = "set "_s;
            break;
        default:
            break;
        }
        return cacheIfNoException(jsMakeNontrivialString(globalObject, before, isNamed ? ecmaName().string() : String(), isNamed ? "() { [native code] }"_s : "() => { [native code] }"_s));
    }
#endif

    if (isClass())
        return cache(jsString(vm, classSource().view()));

    StringView src = source().provider()->getRange(
        functionStart(),
        parametersStartOffset() + source().length());

    return cacheIfNoException(jsMakeNontrivialString(globalObject, src));
}

void FunctionExecutable::overrideInfo(const FunctionOverrideInfo& overrideInfo)
{
    auto& rareData = ensureRareData();
    m_source = overrideInfo.sourceCode;
    rareData.m_lineCount = overrideInfo.lineCount;
    rareData.m_endColumn = overrideInfo.endColumn;
    rareData.m_parametersStartOffset = overrideInfo.parametersStartOffset;
    rareData.m_functionStart = overrideInfo.functionStart;
    rareData.m_functionEnd = overrideInfo.functionEnd;
}

auto FunctionExecutable::ensureTemplateObjectMap(VM&) -> TemplateObjectMap&
{
    RareData& rareData = ensureRareData();
    return ensureTemplateObjectMapImpl(rareData.m_templateObjectMap);
}

} // namespace JSC
