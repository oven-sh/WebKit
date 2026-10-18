/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTImageCompilation.h"

#if ENABLE(AOT)

#include "AOTCompiler.h"
#include "AOTProgram.h"
#include "AOTRuntime.h"
#include "AOTTypedThreads.h"
#include "AOTWebAssembly.h"
#include "B3CompilationCohort.h"
#include "BytecodeStructs.h"
#include "ImmutableIntrinsics.h"
#include "JSCInlines.h"
#include "JSModuleEnvironment.h"
#include "JSTemplateObjectDescriptor.h"
#include "MacroAssembler.h"
#include "PrelinkedModuleGraph.h"
#include "RegExp.h"
#include "SymbolTable.h"
#include "UnlinkedFunctionCodeBlock.h"
#include "UnlinkedFunctionExecutable.h"
#include "UnlinkedModuleProgramCodeBlock.h"
#include <wtf/MonotonicTime.h>
#include <wtf/NumberOfCores.h>
#include <wtf/Threading.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

void ImageCompilation::layOutModuleEnvironments()
{
    using Graph = PrelinkedModuleGraph;
    if (prelinkedGraph.size() < sizeof(Graph::Header))
        return;
    auto& header = *reinterpret_cast<const Graph::Header*>(prelinkedGraph.data());
    auto arrayAt = [&]<typename T>(uint32_t offset, uint32_t count, const T*) -> std::span<const T> {
        RELEASE_ASSERT(offset <= prelinkedGraph.size() && count <= (prelinkedGraph.size() - offset) / sizeof(T));
        return { reinterpret_cast<const T*>(prelinkedGraph.data() + offset), count };
    };
    RELEASE_ASSERT(header.magic == Graph::magic && header.version == Graph::currentVersion);
    auto graphModules = arrayAt(header.modulesOffset, header.moduleCount, static_cast<const Graph::Module*>(nullptr));

    struct Linked {
        UnlinkedModuleProgramCodeBlock* codeBlock { nullptr };
        SymbolTable* symbolTable { nullptr };
        unsigned index { 0 };
    };
    Vector<Linked> linked(graphModules.size());
    for (unsigned index = 0; index < modules.size() && index < graphModuleIndices.size(); ++index) {
        uint32_t graphModule = graphModuleIndices[index];
        auto* codeBlock = dynamicDowncast<UnlinkedModuleProgramCodeBlock>(modules[index].root);
        if (graphModule >= linked.size() || !codeBlock)
            continue;
        JSValue symbolTable = codeBlock->getConstant(VirtualRegister(codeBlock->moduleEnvironmentSymbolTableConstantRegisterOffset()));
        linked[graphModule] = { codeBlock, uncheckedDowncast<SymbolTable>(symbolTable.asCell()), index };
    }

    linkEnvironments.fill(AOT::ImageEnvironment { }, graphModules.size());
    for (unsigned graphModule = 0; graphModule < linked.size(); ++graphModule) {
        if (!linked[graphModule].codeBlock)
            continue;
        size_t size = JSModuleEnvironment::allocationSize(linked[graphModule].symbolTable, graphModules[graphModule].importCount);
        linkEnvironmentsSize += sizeof(void*);
        linkEnvironments[graphModule] = { static_cast<uint32_t>(linkEnvironmentsSize), static_cast<uint32_t>(size) };
    }
}

std::optional<Vector<uint32_t>> ImageCompilation::variablesExportedBy(unsigned index)
{
    using Graph = PrelinkedModuleGraph;
    auto* codeBlock = dynamicDowncast<UnlinkedModuleProgramCodeBlock>(modules[index].root);
    if (!codeBlock || index >= graphModuleIndices.size() || prelinkedGraph.size() < sizeof(Graph::Header))
        return std::nullopt;
    auto& header = *reinterpret_cast<const Graph::Header*>(prelinkedGraph.data());
    RELEASE_ASSERT(header.magic == Graph::magic && header.version == Graph::currentVersion);
    uint32_t graphModule = graphModuleIndices[index];
    if (graphModule >= header.moduleCount)
        return std::nullopt;
    RELEASE_ASSERT(header.modulesOffset <= prelinkedGraph.size() && header.moduleCount <= (prelinkedGraph.size() - header.modulesOffset) / sizeof(Graph::Module));
    RELEASE_ASSERT(header.exportsOffset <= prelinkedGraph.size() && header.exportCount <= (prelinkedGraph.size() - header.exportsOffset) / sizeof(Graph::Export));
    auto& module = reinterpret_cast<const Graph::Module*>(prelinkedGraph.data() + header.modulesOffset)[graphModule];
    RELEASE_ASSERT(module.firstExport <= header.exportCount && module.exportCount <= header.exportCount - module.firstExport);
    auto* exports = reinterpret_cast<const Graph::Export*>(prelinkedGraph.data() + header.exportsOffset) + module.firstExport;
    auto* symbolTable = uncheckedDowncast<SymbolTable>(codeBlock->getConstant(VirtualRegister(codeBlock->moduleEnvironmentSymbolTableConstantRegisterOffset())).asCell());
    Vector<uint32_t> result;
    for (unsigned i = 0; i < module.exportCount; ++i) {
        if (exports[i].kind() != Graph::ExportKind::Local)
            continue;
        uint32_t sid = exports[i].localOrImportSid;
        if (sid == Graph::starDefaultSid)
            continue;
        if (sid >= prelinkedGraphStringSlots.size())
            return std::nullopt;
        String string = strings->stringForSlot(prelinkedGraphStringSlots[sid]);
        if (string.isNull())
            return std::nullopt;
        SymbolTableEntry::Fast entry = symbolTable->get(Identifier::fromString(vm, string).impl());
        if (!entry.isNull() && entry.varOffset().isScope())
            result.append(entry.scopeOffset().offset());
    }
    RELEASE_ASSERT(header.importsOffset <= prelinkedGraph.size() && header.importCount <= (prelinkedGraph.size() - header.importsOffset) / sizeof(Graph::Import));
    RELEASE_ASSERT(module.firstImport <= header.importCount && module.importCount <= header.importCount - module.firstImport);
    auto* imports = reinterpret_cast<const Graph::Import*>(prelinkedGraph.data() + header.importsOffset) + module.firstImport;
    for (unsigned i = 0; i < module.importCount; ++i) {
        uint32_t sid = imports[i].localSid;
        if (sid >= prelinkedGraphStringSlots.size())
            return std::nullopt;
        String string = strings->stringForSlot(prelinkedGraphStringSlots[sid]);
        if (string.isNull())
            return std::nullopt;
        SymbolTableEntry::Fast entry = symbolTable->get(Identifier::fromString(vm, string).impl());
        if (!entry.isNull() && entry.varOffset().isScope())
            result.append(entry.scopeOffset().offset());
    }
    return result;
}

Vector<uint8_t> ImageCompilation::compileImage()
{
    MacroAssembler::restrictToBuildTargetFeatures();
    if (!MacroAssembler::supportsFloatingPointRounding()) {
        fail("this build targets a CPU without SSE4.1, which the ahead-of-time compiler needs"_s);
        return { };
    }
    ImmutableIntrinsics::ensureShared(vm);
    if (omittedFunctions[0] || omittedFunctions[1] || omittedFunctions[2]) {
        fail(makeString("internal error: "_s, omittedFunctions[0], " functions have no bytecode, "_s, omittedFunctions[1], " were not placed and "_s, omittedFunctions[2], " have no key"_s));
        return { };
    }
    for (auto& function : functionsToCompile) {
        for (bool isConstruct : { false, true }) {
            if (isConstruct ? function.forConstruct : function.forCall)
                fail(makeString("internal error: the function `"_s, function.executable->ecmaName().string(), "` is not in the typed program"_s), pathOf(function.module));
        }
    }
    if (!functionsToCompile.isEmpty())
        return { };
    struct Job {
        AOT::ImageKey key;
        uint64_t rank;
        UnlinkedCodeBlock* codeBlock;
        unsigned module;
    };
    Vector<Job> jobs;
    constexpr uint64_t isNotHot = 1ull << 63;
    constexpr uint64_t runsOnce = 1ull << 62;
    auto hasNoEffects = [](UnlinkedCodeBlock* codeBlock) {
        for (const auto& instruction : codeBlock->instructions()) {
            OpcodeID opcode = instruction->opcodeID();
            if (opcode != op_enter && opcode != op_mov && opcode != op_ret)
                return false;
        }
        return true;
    };
    bool isAllInTypedProgram = true;
    for (unsigned index = 0; index < modules.size(); ++index) {
        auto* codeBlock = dynamicDowncast<UnlinkedCodeBlock>(modules[index].root);
        if (!codeBlock)
            continue;
        if (typedModules.contains(index)) {
            jobs.append({ AOT::imageKeyForTopLevelCode(modules[index].entryOffset + 1), (hasHotFunctions && !modules[index].isLate ? runsOnce : isNotHot) | static_cast<uint64_t>(index) << 34, codeBlock, index });
            continue;
        }
        if (hasNoEffects(codeBlock))
            continue;
        fail("internal error: the module is not in the typed program"_s, pathOf(index));
        isAllInTypedProgram = false;
    }
    if (!isAllInTypedProgram)
        return { };

    auto moduleSource = [&](unsigned module) -> StringView {
        return modules[module].source.provider()->source();
    };

    layOutModuleEnvironments();
    unsigned numberOfThreads = Options::numberOfAOTCompilerThreads() ? Options::numberOfAOTCompilerThreads() : WTF::numberOfProcessorCores();
    DeferGC deferGC(vm);
    MonotonicTime phaseStart = MonotonicTime::now();
    MonotonicTime stepStart = phaseStart;
    AOT::Typed::PhaseTimes phaseTimes("compile"_s);
    auto endPhase = [&](ASCIILiteral name) {
        MonotonicTime now = MonotonicTime::now();
        if (AOT::Typed::shouldPrintTimings()) [[unlikely]]
            dataLogLn("AOT: ", name, " took ", static_cast<unsigned>((now - phaseStart).milliseconds()), " ms");
        phaseStart = now;
        stepStart = now;
        phaseTimes.didFinish(name);
    };
    auto endStep = [&](ASCIILiteral name) {
        if (!AOT::Typed::shouldPrintTimings()) [[likely]]
            return;
        MonotonicTime now = MonotonicTime::now();
        dataLogLn("AOT: - ", name, " took ", static_cast<unsigned>((now - stepStart).milliseconds()), " ms");
        stepStart = now;
        phaseTimes.didFinish(name);
    };
    AOT::ImageBuilder builder;
    builder.setEnvironments(WTF::move(linkEnvironments), safeCast<uint32_t>(linkEnvironmentsSize));
    if (discoveredRegExps)
        builder.takePrecompiledRegExps(discoveredRegExps->precompiledRegExps());
    UncheckedKeyHashSet<UnlinkedCodeBlock*> declined;
    auto typedEntryOf = [&](size_t index) -> AOT::Typed::FunctionID {
        if (!typedProgram)
            return { };
        size_t entry = typedModules.find(jobs[index].module);
        return entry == notFound ? AOT::Typed::FunctionID() : typedProgram->entries[entry];
    };
    auto isTypedEntry = [&](size_t index) {
        return !!typedEntryOf(index);
    };
    Vector<String> typedFunctionsForMap;
    if (typedProgram && Options::aotMapFilePath()) [[unlikely]]
        typedFunctionsForMap = AOT::Typed::describeForMap(*typedProgram);
    auto compileJob = [&](size_t index) {
        AOT::CompiledCode code;
        if (Options::aotFunctionToDump()) [[unlikely]] {
            StringView wanted = StringView::fromLatin1(byteCast<char>(Options::aotFunctionToDump()));
            code.mayBeDumped = makeString(jobs[index].key.start) == wanted || makeString(jobs[index].key.module, ':', jobs[index].key.start) == wanted;
        }
        if (!isTypedEntry(index) || !AOT::Typed::compile(vm, *typedProgram, typedEntryOf(index), code)) {
            declined.add(jobs[index].codeBlock);
            fail(makeString("internal error: the entry function of a module was not compiled"_s, code.declineReason.isEmpty() ? ""_s : ": "_s, code.declineReason), pathOf(jobs[index].module));
            return;
        }
        code.info.convention = AOT::conventionOf(jobs[index].codeBlock);
        AOT::collectConstructSites(code.info, jobs[index].codeBlock, moduleSource(jobs[index].module), 0);
        AOT::collectQuotes(code.info, jobs[index].codeBlock, moduleSource(jobs[index].module), 0);
        if (auto* numbers = AOT::programIdentifierIndices()) {
            for (auto& identifier : jobs[index].codeBlock->identifiers())
                code.info.identifierIndices.append(numbers->get(identifier.impl()));
        }
        if (auto* numbers = AOT::programConstantIndicesFor(jobs[index].codeBlock))
            code.info.constantIndices = *numbers;
        String nameForMap;
        if (Options::aotMapFilePath()) [[unlikely]]
            nameForMap = typedFunctionsForMap[typedEntryOf(index).index()].isolatedCopy();
        builder.add(jobs[index].key, jobs[index].rank, WTF::move(code), WTF::move(nameForMap));
    };
    AOT::IdentifierIndices identifierIndices;
    Vector<std::pair<AOT::Typed::StringID, Identifier>> typedIdentifiers;
    Vector<std::array<uint32_t, 4>> typedIdentifierUsesByAccess;
    AOT::IdentifierIndices firstFunctionsUsingLookupTablePlusOne;
    {
        for (auto& job : jobs) {
            for (auto& identifier : job.codeBlock->identifiers())
                identifierIndices.add(identifier.impl(), 0).iterator->value++;
        }
        if (typedProgram) {
            typedProgram->findPossibleAccessorNames();
            for (auto& [name, uses, tableLookupUses, firstTableLookupFunction, usesByAccess] : typedProgram->genericAccessNameUses()) {
                auto nameBytes = typedProgram->string(name);
                Identifier identifier = AOT::Typed::isConstantSymbol(nameBytes) || AOT::Typed::isEnginePrivateName(nameBytes) ? Identifier::fromUid(SymbolImpl::create(*String(byteCast<Latin1Character>(nameBytes)).impl()).get()) : Identifier::fromString(vm, String::fromUTF8(nameBytes));
                identifierIndices.add(identifier.impl(), 0).iterator->value += std::min<uint32_t>(uses, INT16_MAX) + (std::min<uint32_t>(tableLookupUses, INT16_MAX) << 16);
                if (firstTableLookupFunction != UINT32_MAX) {
                    uint32_t& first = firstFunctionsUsingLookupTablePlusOne.add(identifier.impl(), firstTableLookupFunction + 1).iterator->value;
                    first = std::min(first, firstTableLookupFunction + 1);
                }
                typedIdentifiers.append({ name, WTF::move(identifier) });
                typedIdentifierUsesByAccess.append(usesByAccess);
            }
        }
        Vector<std::pair<UniquedStringImpl*, uint32_t>> inOrder;
        for (auto& entry : identifierIndices)
            inOrder.append({ entry.key, entry.value });
        AOT::Typed::parallelSort(inOrder.mutableSpan(), [](auto& a, auto& b) {
            if (a.second != b.second)
                return a.second > b.second;
            if (auto order = codePointCompare(StringView { *a.first }, StringView { *b.first }); order != std::weak_ordering::equivalent)
                return order == std::weak_ordering::less;
            return a.first->isSymbol() < b.first->isSymbol();
        });
        if (constexpr size_t hotNameCount = AOT::PropertyLookupTable::numberOfHotNames - 1; inOrder.size() > hotNameCount) {
            auto remainingNamesWithColumn = inOrder.mutableSpan().subspan(hotNameCount, std::min<size_t>(inOrder.size(), AOT::PropertyLookupTable::maxName) - hotNameCount);
            for (auto& [uid, key] : remainingNamesWithColumn)
                key = firstFunctionsUsingLookupTablePlusOne.get(uid) - 1;
            std::stable_sort(remainingNamesWithColumn.begin(), remainingNamesWithColumn.end(), [](auto& a, auto& b) {
                return a.second < b.second;
            });
        }
        for (uint32_t number = 0; number < inOrder.size(); ++number)
            identifierIndices.set(inOrder[number].first, number + 1);
        AOT::clearNamesWithDedicatedStubs();
        using AccessByName = AOT::Typed::Program::AccessByName;
        static_assert(static_cast<unsigned>(AccessByName::Read) == static_cast<unsigned>(AOT::PerNameStubKind::Reads) && static_cast<unsigned>(AccessByName::Store) == static_cast<unsigned>(AOT::PerNameStubKind::Stores));
        static_assert(static_cast<unsigned>(AccessByName::Definition) == static_cast<unsigned>(AOT::PerNameStubKind::Definitions) && static_cast<unsigned>(AccessByName::Call) == static_cast<unsigned>(AOT::PerNameStubKind::Calls));
        for (unsigned i = 0; i < typedIdentifiers.size(); ++i) {
            constexpr uint32_t minUsesForDedicatedStub = 512;
            uint32_t number = identifierIndices.get(typedIdentifiers[i].second.impl());
            for (unsigned access = 0; access < typedIdentifierUsesByAccess[i].size(); ++access) {
                if (typedIdentifierUsesByAccess[i][access] >= minUsesForDedicatedStub)
                    AOT::setHasDedicatedStub(static_cast<AOT::PerNameStubKind>(access), number);
            }
            if (AOT::collectionMethodNamed(typedProgram->string(typedIdentifiers[i].first)) != AOT::CollectionMethod::None)
                AOT::setHasDedicatedStub(AOT::PerNameStubKind::Calls, number);
        }
        AOT::setTypedStringsWithWord(typedProgram ? typedProgram->mostUsedStringKeys(AOT::Instance::numberOfTypedStringsWithWord) : Vector<uint32_t>());
        AOT::setProgramIdentifierIndices(&identifierIndices);
        builder.setProgramIdentifierIndices(&identifierIndices, inOrder.size() + 1);
        if (typedProgram) {
            typedProgram->identifierNumbers.fill(0, typedProgram->strings.size());
            for (auto& [name, identifier] : typedIdentifiers)
                typedProgram->identifierNumbers[name.index()] = identifierIndices.get(identifier.impl());
        }
    }

    endStep("numbering the identifiers"_s);
    AOT::ConstantIndices constantIndices;
    if (AOT::programIdentifierIndices()) {
        UncheckedKeyHashMap<String, uint32_t> strings;
        uint64_t cells = 0;
        UncheckedKeyHashMap<uint64_t, uint32_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> others;
        uint32_t next = 0;
        UncheckedKeyHashMap<uint64_t, uint32_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> templates;
        for (auto& job : jobs) {
            Vector<uint32_t> numbers;
            auto& representations = job.codeBlock->constantsSourceCodeRepresentation();
            for (auto& constant : job.codeBlock->constantRegisters()) {
                JSValue value = constant.get();
                uint32_t number = AOT::invalidConstantIndex;
                auto take = [&] {
                    return next++;
                };
                if (!value || representations[numbers.size()] == SourceCodeRepresentation::LinkTimeConstant) {
                } else if (!value.isCell())
                    number = others.ensure(static_cast<uint64_t>(JSValue::encode(value)), take).iterator->value;
                else {
                    String said;
                    if (value.isString())
                        said = asString(value)->tryGetValue();
                    if (!said.isNull())
                        number = strings.ensure(said, take).iterator->value;
                    else if (auto* descriptor = dynamicDowncast<JSTemplateObjectDescriptor>(value.asCell()))
                        number = templates.ensure(static_cast<uint64_t>(job.module) << 32 | static_cast<uint32_t>(descriptor->endOffset()), take).iterator->value;
                    else {
                        number = take();
                        ++cells;
                    }
                }
                numbers.append(number);
            }
            constantIndices.add(job.codeBlock, WTF::move(numbers));
        }
        {
            Vector<uint32_t> uses;
            uses.fill(0, next);
            for (auto& entry : constantIndices) {
                for (uint32_t number : entry.value) {
                    if (number != AOT::invalidConstantIndex)
                        uses[number]++;
                }
            }
            Vector<uint32_t> inOrder;
            for (uint32_t i = 0; i < next; ++i)
                inOrder.append(i);
            std::stable_sort(inOrder.begin(), inOrder.end(), [&](uint32_t a, uint32_t b) { return uses[a] > uses[b]; });
            Vector<uint32_t> rank;
            rank.fill(0, next);
            uint64_t within = 0;
            for (uint32_t i = 0; i < next; ++i) {
                rank[inOrder[i]] = i;
                if (i < 4096)
                    within += uses[inOrder[i]];
            }
            for (auto& entry : constantIndices) {
                for (uint32_t& number : entry.value) {
                    if (number != AOT::invalidConstantIndex)
                        number = rank[number];
                }
            }
        }
        AOT::setProgramConstantIndices(&constantIndices);
        builder.setNumberOfProgramConstants(next);
    }

    endStep("numbering the constants"_s);
    if (typedProgram) {
        struct TypedResult {
            WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(TypedResult);
            AOT::CompiledCode code;
            bool isCompiled { false };
        };
        size_t numberOfEntriesWithModule = 0;
        for (size_t index = 0; index < jobs.size(); ++index) {
            if (isTypedEntry(index)) {
                compileJob(index);
                ++numberOfEntriesWithModule;
            }
        }
        if (!declined.isEmpty())
            return { };
        if (numberOfEntriesWithModule != typedProgram->entries.size()) {
            fail(makeString("internal error: the typed program has "_s, typedProgram->entries.size(), " entry functions for "_s, numberOfEntriesWithModule, " modules"_s));
            return { };
        }
        uint32_t numberOfTypedFunctions = typedProgram->functions.size();
        Vector<std::unique_ptr<TypedResult>> typedResults(numberOfTypedFunctions);
        std::atomic<uint32_t> nextTypedFunction { 0 };
        constexpr size_t maxValuesToOrderByShape = 64;
        Vector<std::pair<uint64_t, uint32_t>> sizesAndIndices(numberOfTypedFunctions);
        AOT::Typed::forEachIndexInParallel(numberOfTypedFunctions, [&](unsigned index) {
            auto& function = std::as_const(*typedProgram).functions[index];
            uint32_t shapeHash = function.values.size() <= maxValuesToOrderByShape ? AOT::Typed::shapeHashForCompilationOrder(function) : 0;
            sizesAndIndices[index] = { static_cast<uint64_t>(UINT32_MAX - function.values.size()) << 32 | shapeHash, index };
        });
        AOT::Typed::parallelSort(sizesAndIndices.mutableSpan(), std::less<std::pair<uint64_t, uint32_t>> { });
        Vector<uint32_t> largestFirst;
        largestFirst.reserveInitialCapacity(numberOfTypedFunctions);
        for (auto [sizeAndShape, number] : sizesAndIndices) {
            AOT::Typed::FunctionID id(number);
            if (!typedProgram->isEntry(id) && typedProgram->hasCode(id))
                largestFirst.append(id.index());
        }
        auto nameOf = [&](AOT::Typed::FunctionID id) {
            return String::fromUTF8(typedProgram->string(typedProgram->at(id).name));
        };
        auto releaseMemory = [](auto& self, AOT::CompiledCode& code) -> void {
            code.bytes.shrinkToFit();
            code.instructions.shrinkToFit();
            code.dataInCode.shrinkToFit();
            auto& info = code.info;
            info.indexRelocations.shrinkToFit();
            info.spreadSites.shrinkToFit();
            info.inlineFrames.shrinkToFit();
            info.stubCalls.shrinkToFit();
            info.argumentMoves.shrinkToFit();
            info.sites.shrinkToFit();
            info.knownCallees.shrinkToFit();
            info.siteConstants.shrinkToFit();
            info.plans.shrinkToFit();
            info.quotableSites.shrinkToFit();
            info.constructSites.shrinkToFit();
            info.constructingSites.shrinkToFit();
            info.callSites.shrinkToFit();
            info.identifierIndices.shrinkToFit();
            info.typedIdentifierIndices.shrinkToFit();
            info.typedIdentifierNames.shrinkToFit();
            info.typedCatchEntrypointsForCallSites.shrinkToFit();
            info.faultingAccesses.shrinkToFit();
            info.typedPositions.shrinkToFit();
            info.typedSourceNames.shrinkToFit();
            info.constantIndices.shrinkToFit();
            info.constructionStarts.shrinkToFit();
            info.quotes.shrinkToFit();
            for (auto& continuation : code.continuations)
                self(self, continuation);
            code.continuations.shrinkToFit();
        };
        auto compileTypedFunction = [&](uint32_t function) {
            AOT::Typed::FunctionID id(function);
            auto result = makeUnique<TypedResult>();
            if (Options::aotFunctionToDump()) [[unlikely]]
                result->code.mayBeDumped = nameOf(id) == StringView::fromLatin1(byteCast<char>(Options::aotFunctionToDump()));
            result->isCompiled = AOT::Typed::compile(vm, *typedProgram, id, result->code);
            if (result->isCompiled)
                AOT::ImageBuilder::clearStubCalls(result->code);
            releaseMemory(releaseMemory, result->code);
            typedResults[function] = WTF::move(result);
        };
        auto cohortSizeAt = [&](uint32_t next) -> uint32_t {
            constexpr size_t maxNumberOfValuesInCohort = 500;
            if (next >= largestFirst.size())
                return 1;
            size_t numberOfValues = std::max<size_t>(typedProgram->functions[largestFirst[next]].values.size(), 1);
            return std::clamp<size_t>(maxNumberOfValuesInCohort / numberOfValues, 1, B3::CompilationCohort::maxSize);
        };
        auto compileTypedFunctions = [&] {
            B3::CompilationCohort cohort;
            AOT::Typed::CompiledProcedureCache compiledProcedures;
            for (;;) {
                if (builder.compileOneRequestedRegExp())
                    continue;
                uint32_t size = cohortSizeAt(nextTypedFunction.load(std::memory_order_relaxed));
                uint32_t first = nextTypedFunction.fetch_add(size);
                if (first >= largestFirst.size())
                    break;
                size = std::min<uint32_t>(size, largestFirst.size() - first);
                if (size == 1) {
                    compileTypedFunction(largestFirst[first]);
                    continue;
                }
                cohort.run(size, [&](unsigned member) {
                    compileTypedFunction(largestFirst[first + member]);
                });
            }
            builder.compileRegExpsUntilAllAreRequested();
        };
        endStep("the entries, and the functions by size"_s);
        for (uint32_t module : typedModules)
            typedProgram->entryModules.append(modules[module].entryOffset);
        {
            Vector<Ref<Thread>> threads;
            for (unsigned i = 1; i < numberOfThreads; ++i)
                threads.append(Thread::create("AOT compiler"_s, [&compileTypedFunctions] { compileTypedFunctions(); }));
            Vector<uint8_t> typedData;
            Ref encoder = Thread::create("AOT data encoder"_s, [&] {
                typedData = AOT::Typed::encodeData(*typedProgram);
            });
            endStep("starting the threads"_s);
            AOT::Typed::addRegExpLiterals(vm, *typedProgram, builder);
            if (!discoveredRegExps)
                discoveredRegExps = makeUnique<AOT::DiscoveredRegExps>(typedProgram->buffer);
            auto addEvaluatedRegExps = [&] {
                for (auto& [source, flags] : discoveredRegExps->waitForEvaluatedSources())
                    builder.requestRegExp(vm, source, flags, true);
                builder.didRequestAllRegExps();
            };
            RefPtr<Thread> waitsForEvaluator;
            if (discoveredRegExps->isEvaluated())
                addEvaluatedRegExps();
            else
                waitsForEvaluator = Thread::create("AOT compiler"_s, [&addEvaluatedRegExps] { addEvaluatedRegExps(); });
            endStep("addRegExpLiterals"_s);
            endPhase("regular expressions of the typed program"_s);
            compileTypedFunctions();
            endStep("the share of this thread in the code generation"_s);
            encoder->waitForCompletion();
            endStep("waiting for the data encoder"_s);
            builder.setTypedData(WTF::move(typedData));
            for (auto& thread : threads)
                thread->waitForCompletion();
            if (waitsForEvaluator)
                waitsForEvaluator->waitForCompletion();
            AOT::Typed::destroyOnHelperThread(WTF::move(discoveredRegExps));
        }
        endStep("waiting for the other threads"_s);
        RELEASE_ASSERT(typedProgram->sizeOfStatics == typedProgram->placedStaticsSize);
        endPhase("code generation"_s);
        if (Options::verboseAOTCompilation()) [[unlikely]] {
            uint64_t total = 0;
            uint64_t count = 0;
            std::array<uint64_t, 24> bytesBySize { };
            std::array<uint64_t, 24> countBySize { };
            for (auto& result : typedResults) {
                if (!result || !result->isCompiled)
                    continue;
                size_t size = result->code.bytes.size();
                total += size;
                ++count;
                unsigned bucket = std::min<unsigned>(23, size ? WTF::fastLog2(static_cast<unsigned>(size)) : 0);
                bytesBySize[bucket] += size;
                ++countBySize[bucket];
            }
            dataLogLn("AOT statistics: code of typed functions: ", total, " bytes in ", count, " functions");
            for (unsigned bucket = 0; bucket < bytesBySize.size(); ++bucket) {
                if (countBySize[bucket])
                    dataLogLn("AOT statistics: functions of up to ", 1u << bucket, " bytes: ", countBySize[bucket], ", ", bytesBySize[bucket], " bytes");
            }
        }
        unsigned numberOfDeclinedFunctions = 0;
        Vector<uint64_t> typedFunctionOrder(numberOfTypedFunctions, [](size_t function) {
            return static_cast<uint64_t>(function) << 2;
        });
        for (auto& [function, entry] : typedProgram->genericEntrypoints) {
            if (function)
                typedFunctionOrder[entry.index()] = (static_cast<uint64_t>(function) << 2) - 1;
        }
        Vector<size_t> builderIndices(numberOfTypedFunctions);
        size_t numberToAdd = 0;
        struct SortKey {
            uint64_t rank;
            uint32_t start;
            uint32_t kind;
            uint32_t place;
        };
        Vector<SortKey> orderedLocations;
        orderedLocations.reserveInitialCapacity(numberOfTypedFunctions);
        constexpr uint32_t isNotCompiled = std::numeric_limits<uint32_t>::max();
        Vector<uint32_t> numbersAdded(numberOfTypedFunctions);
        AOT::Typed::forEachIndexInParallel(numberOfTypedFunctions, [&](unsigned function) {
            auto& result = typedResults[function];
            numbersAdded[function] = !result ? 0 : !result->isCompiled ? isNotCompiled : safeCast<uint32_t>(result->code.continuations.size() + 1);
        });
        for (uint32_t function = 0; function < numberOfTypedFunctions; ++function) {
            builderIndices[function] = numberToAdd;
            if (!numbersAdded[function])
                continue;
            AOT::Typed::FunctionID id(function);
            if (numbersAdded[function] == isNotCompiled) {
                auto& result = typedResults[function];
                auto& declined = typedProgram->at(id);
                LineColumn position = typedProgram->lineColumnFor(declined.module, declined.position);
                String path = declined.module ? String::fromUTF8(typedProgram->string(typedProgram->modules[declined.module.index()].displayPath)) : String();
                bool isModuleBody = declined.kind == AOT::Typed::Format::FunctionKind::Module || declined.has(AOT::Typed::Format::FunctionIsModuleBody);
                String what = isModuleBody ? String("the top-level code"_s) : makeString('`', nameOf(id), '`');
                fail(makeString(what, " cannot be compiled ahead of time: "_s, result->code.declineReason), WTF::move(path), position);
                ++numberOfDeclinedFunctions;
                continue;
            }
            for (unsigned index = 0; index + 1 < numbersAdded[function]; ++index) {
                auto key = AOT::Typed::continuationKey(id, index);
                RELEASE_ASSERT(!key.module);
                orderedLocations.append({ isNotHot | 1ull << 61 | typedFunctionOrder[function] | 1, key.start, key.kind, safeCast<uint32_t>(numberToAdd++) });
            }
            auto key = AOT::Typed::keyOf(id);
            RELEASE_ASSERT(!key.module);
            orderedLocations.append({ isNotHot | 1ull << 61 | typedFunctionOrder[function], key.start, key.kind, safeCast<uint32_t>(numberToAdd++) });
        }
        if (numberOfDeclinedFunctions)
            return { };
        AOT::Typed::parallelSort(orderedLocations.mutableSpan(), [](const SortKey& a, const SortKey& b) {
            return std::tie(a.rank, a.start, a.kind) < std::tie(b.rank, b.start, b.kind);
        });
        Vector<uint32_t> finalIndices(numberToAdd);
        for (size_t index = 0; index < orderedLocations.size(); ++index)
            finalIndices[orderedLocations[index].place] = safeCast<uint32_t>(index);
        size_t firstBuilderIndex = builder.growFunctions(numberToAdd);
        AOT::Typed::forEachIndexInParallel(numberOfTypedFunctions, [&](unsigned function) {
            auto result = WTF::move(typedResults[function]);
            if (!result)
                return;
            AOT::Typed::FunctionID id(function);
            size_t place = builderIndices[function];
            String nameForMap = typedFunctionsForMap.isEmpty() ? String() : WTF::move(typedFunctionsForMap[function]);
            auto continuations = WTF::move(result->code.continuations);
            for (unsigned index = 0; index < continuations.size(); ++index)
                builder.set(firstBuilderIndex + finalIndices[place++], AOT::Typed::continuationKey(id, index), isNotHot | 1ull << 61 | typedFunctionOrder[function] | 1, WTF::move(continuations[index]), makeStringByReplacingAll(nameForMap, "\ttyped\t"_s, "\tcontinuation\t"_s));
            builder.set(firstBuilderIndex + finalIndices[place], AOT::Typed::keyOf(id), isNotHot | 1ull << 61 | typedFunctionOrder[function], WTF::move(result->code), WTF::move(nameForMap));
        });
        endPhase("adding the functions to the image"_s);
        AOT::addWebAssemblyModules(vm, *typedProgram, builder);
        endPhase("WebAssembly"_s);
        AOT::Typed::destroyOnHelperThread(WTF::move(typedProgram));
        endPhase("freeing the typed functions"_s);
    }
    AOT::setProgramIdentifierIndices(nullptr);
    AOT::setProgramConstantIndices(nullptr);
    for (auto& job : jobs) {
        for (auto& constant : job.codeBlock->constantRegisters()) {
            if (auto* regExp = constant.get().isCell() ? dynamicDowncast<RegExp>(constant.get().asCell()) : nullptr)
                builder.requestRegExp(vm, regExp->pattern(), regExp->flags());
        }
    }
    endPhase("regular expressions"_s);
    Vector<uint8_t> image = builder.finish();
    if (!builder.failureReason().isNull())
        fail(builder.failureReason());
    endPhase("writing the image"_s);
    reportableSites = builder.takeReportableSites();
    return image;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
