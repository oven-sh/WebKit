/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTProgramData.h"

#if ENABLE(AOT)

#include "AOTImage.h"
#include "AOTProgram.h"
#include "AOTRuntime.h"
#include "BuiltinExecutables.h"
#include "BytecodeStructs.h"
#include "CachedTypes.h"
#include "FunctionExecutable.h"
#include "JSBigInt.h"
#include "JSCInlines.h"
#include "JSTemplateObjectDescriptor.h"
#include "SourceCodeKey.h"
#include "UnlinkedFunctionCodeBlock.h"
#include "UnlinkedFunctionExecutable.h"
#include "UnlinkedModuleProgramCodeBlock.h"
#include <wtf/BitVector.h>
#include <wtf/OSAllocator.h>
#include <wtf/TZoneMallocInlines.h>

namespace JSC { namespace AOT {

WTF_MAKE_TZONE_ALLOCATED_IMPL(VMProgram);

namespace {

void appendVarint(Vector<uint8_t>& bytes, uint64_t value)
{
    while (value >= 0x80) {
        bytes.append(static_cast<uint8_t>(value) | 0x80);
        value >>= 7;
    }
    bytes.append(static_cast<uint8_t>(value));
}

struct DynamicallyResolvedNames {
    bool mayBeAnything { false };
    UncheckedKeyHashSet<UniquedStringImpl*> names;
};

bool isLookedUpByName(ResolveType type)
{
    return type != ResolvedClosureVar && type != ResolvedLazyClosureVar && type != ModuleVar && !isStaticClosureVarResolveType(type);
}

class Builder {
public:
    Builder(VM& vm, const ImageView& image, const ProgramData::RetainedPositions& positions, std::span<const FunctionReportableSites> reportableSites)
        : m_vm(vm)
        , m_image(image)
        , m_positions(positions)
        , m_reportableSites(reportableSites)
    {
        m_out.fill(0, roundUpToMultipleOf<16>(sizeof(ProgramData)));
        m_infos.grow(image.numberOfFunctions());
        zeroSpan(m_infos.mutableSpan());
        m_functionMetadataOffsets.fill(0, image.numberOfFunctions());
        m_identifiers.fill(nullptr, image.numberOfProgramIdentifiers());
        m_constants.fill(JSValue(), image.numberOfProgramConstants());
        RELEASE_ASSERT(image.numberOfProgramIdentifiers() && reportableSites.size() == image.numberOfFunctions());
    }

    uint32_t append(std::span<const uint8_t> bytes, size_t alignment)
    {
        while (m_out.size() % alignment)
            m_out.append(0);
        uint32_t offset = safeCast<uint32_t>(m_out.size());
        m_out.append(bytes);
        return offset;
    }
    template<typename T> uint32_t append(std::span<T> values) { return append(asBytes(values), alignof(T)); }
    template<typename T, size_t n> uint32_t append(const Vector<T, n>& values) { return append(asBytes(values.span()), alignof(T)); }

    uint32_t appendOnce(std::span<const uint8_t> bytes, size_t alignment)
    {
        uint64_t hash = 1469598103934665603ull;
        for (uint8_t byte : bytes)
            hash = (hash ^ byte) * 1099511628211ull;
        auto& withHash = m_copies.add(hash | 1, Vector<std::pair<uint32_t, uint32_t>, 1> { }).iterator->value;
        for (auto [offset, size] : withHash) {
            if (size == bytes.size() && !(offset % alignment) && equalSpans(m_out.span().subspan(offset, size), bytes))
                return offset;
        }
        uint32_t offset = append(bytes, alignment);
        withHash.append({ offset, safeCast<uint32_t>(bytes.size()) });
        return offset;
    }

    uint32_t numberOfIdentifier(UniquedStringImpl* name)
    {
        if (!name)
            return 0;
        return m_identifierIndices.ensure(name, [&] {
            m_identifiers.append(name);
            return safeCast<uint32_t>(m_identifiers.size() - 1);
        }).iterator->value;
    }

    uint32_t numberOfConstant(JSValue value)
    {
        return m_otherConstantIndices.ensure(JSValue::encode(value), [&] {
            m_constants.append(value);
            return safeCast<uint32_t>(m_constants.size() - 1);
        }).iterator->value;
    }

    uint32_t makePositions(uint32_t index, UnlinkedCodeBlock* codeBlock, unsigned sourceOffset, LineStartTable& lineStarts)
    {
        Vector<uint8_t> stream;
        auto& sites = m_positions.sites[index];
        Vector<uint32_t> offsets = sites.offsets;
        if (size_t count = codeBlock->numberOfUnlinkedSwitchJumpTables(); count && isAsyncFunctionBodyParseMode(codeBlock->parseMode())) {
            auto& table = codeBlock->unlinkedSwitchJumpTable(count - 1);
            for (int32_t offset : table.m_branchOffsets)
                offsets.append(std::max(offset ? offset : table.m_defaultOffset, 0));
            std::ranges::sort(offsets);
            offsets.shrink(std::ranges::unique(offsets).begin() - offsets.begin());
        }
        if (!codeBlock->hasExpressionInfo())
            offsets.clear();
        auto positionOf = [&](unsigned offset) {
            LineColumn inText = lineStarts.lineColumnForOffset(StringView(), offset);
            return LineColumn { inText.line + 1, inText.column + 1 };
        };
        LineColumn start = positionOf(sourceOffset);
        appendVarint(stream, start.line);
        appendVarint(stream, start.column);
        appendVarint(stream, offsets.size());
        uint32_t previousOffset = 0;
        int64_t previousLine = 0;
        int64_t previousColumn = 0;
        uint32_t previousSource = 0;
        auto zigZagEncode = [](int64_t value) {
            return static_cast<uint64_t>(value << 1) ^ static_cast<uint64_t>(value >> 63);
        };
        auto appendPosition = [&](LineColumn inModule) {
            CString name;
            LineColumn position = inModule;
            uint32_t source = 0;
            if (m_positions.find(m_moduleEntryOffset, inModule, name, position)) {
                source = m_sources.ensure(name, [&] {
                    m_sourceNames.append(name);
                    return static_cast<uint32_t>(m_sourceNames.size());
                }).iterator->value;
            } else
                position = inModule;
            if (position.line == previousLine && source == previousSource)
                appendVarint(stream, zigZagEncode(static_cast<int64_t>(position.column) - previousColumn) << 1 | 1);
            else {
                appendVarint(stream, zigZagEncode(static_cast<int64_t>(position.line) - previousLine) << 2 | (source != previousSource) << 1);
                if (source != previousSource)
                    appendVarint(stream, source);
                appendVarint(stream, position.column);
            }
            previousLine = position.line;
            previousColumn = position.column;
            previousSource = source;
        };
        size_t nextConstruction = 0;
        for (uint32_t offset : offsets) {
            if (offset >= codeBlock->instructions().size())
                offset = 0;
            while (nextConstruction < sites.constructions.size() && sites.constructions[nextConstruction].offset < offset)
                ++nextConstruction;
            const auto* construction = nextConstruction < sites.constructions.size() && sites.constructions[nextConstruction].offset == offset ? &sites.constructions[nextConstruction] : nullptr;
            LineColumn position = positionOf(sourceOffset + codeBlock->expressionInfoForBytecodeIndex(BytecodeIndex(offset)).divot);
            appendVarint(stream, static_cast<uint64_t>(offset - previousOffset) << 1 | !!construction);
            previousOffset = offset;
            appendPosition(position);
            if (construction) {
                LineColumn start = position;
                if (construction->linesUp) {
                    start.line = position.line > construction->linesUp ? position.line - construction->linesUp : 1;
                    start.column = construction->columnOrColumnDelta;
                } else if (position.column > construction->columnOrColumnDelta)
                    start.column = position.column - construction->columnOrColumnDelta;
                appendPosition(start);
            }
        }
        return append(stream.span(), 2);
    }

    uint32_t appendFunctionList(std::span<const WriteBarrier<UnlinkedFunctionExecutable>> functions)
    {
        Vector<uint32_t> zeros;
        zeros.fill(0, functions.size());
        uint32_t offset = append(zeros);
        Vector<UnlinkedFunctionExecutable*> list;
        for (auto& function : functions)
            list.append(function.get());
        m_functionLists.append({ offset, WTF::move(list) });
        return offset;
    }

    void fillMetadata(uint32_t index, UnlinkedCodeBlock* codeBlock, ScriptExecutable* executable, LineStartTable& lineStarts)
    {
        if (m_functionMetadataOffsets[index])
            return;
        using Metadata = FunctionMetadata;
        RELEASE_ASSERT(codeBlock->instructions().size() < (1u << (32 - Metadata::instructionsSizeShift)));
        Vector<uint32_t, 16> words { static_cast<uint32_t>(codeBlock->instructions().size()) << Metadata::instructionsSizeShift | (codeBlock->isBuiltinFunction() ? Metadata::isBuiltinFunction : 0) };
        if (executable || codeBlock->codeType() != FunctionCode) {
            words[0] |= Metadata::ExpressionInfo;
            words.append(makePositions(index, codeBlock, executable ? executable->source().startOffset() : 0, lineStarts));
        }
        if (size_t count = codeBlock->numberOfExceptionHandlers()) {
            Vector<UnlinkedHandlerInfo> handlers;
            handlers.grow(count);
            zeroSpan(handlers.mutableSpan());
            for (size_t i = 0; i < count; ++i) {
                const UnlinkedHandlerInfo& handler = codeBlock->exceptionHandler(i);
                handlers[i].start = handler.start;
                handlers[i].end = handler.end;
                handlers[i].target = handler.target;
                handlers[i].typeBits = handler.typeBits;
            }
            words[0] |= Metadata::Handlers;
            words.append(append(handlers));
            words.append(count);
        }
        auto functions = [&](Metadata::Section section, std::span<const WriteBarrier<UnlinkedFunctionExecutable>> all) {
            if (all.empty())
                return;
            words[0] |= section;
            words.append(appendFunctionList(all));
            words.append(all.size());
        };
        functions(Metadata::FunctionDecls, codeBlock->functionDecls());
        functions(Metadata::FunctionExprs, codeBlock->functionExprs());
        if (size_t count = codeBlock->numberOfUnlinkedStringSwitchJumpTables()) {
            Vector<uint32_t> list { static_cast<uint32_t>(count) };
            for (size_t i = 0; i < count; ++i) {
                auto& table = codeBlock->unlinkedStringSwitchJumpTable(i);
                Vector<std::pair<StringImpl*, int32_t>> inOrder;
                for (auto& entry : table.m_offsetTable)
                    inOrder.append({ entry.key.get(), entry.value.m_branchOffset });
                std::ranges::sort(inOrder, [](auto& a, auto& b) { return codePointCompare(StringView { *a.first }, StringView { *b.first }) < 0; });
                Vector<std::pair<uint32_t, int32_t>> entries;
                for (auto [string, offset] : inOrder) {
                    m_switchAtoms.append(AtomStringImpl::add(string));
                    entries.append({ numberOfIdentifier(m_switchAtoms.last().get()), offset });
                }
                std::ranges::sort(entries);
                list.append(entries.size());
                list.append(static_cast<uint32_t>(table.m_defaultOffset));
                for (auto [name, offset] : entries) {
                    list.append(name);
                    list.append(static_cast<uint32_t>(offset));
                }
            }
            words[0] |= Metadata::StringSwitchJumpTables;
            words.append(append(list));
        }
        if (size_t count = codeBlock->numberOfUnlinkedSwitchJumpTables(); count && isAsyncFunctionBodyParseMode(codeBlock->parseMode())) {
            auto& table = codeBlock->unlinkedSwitchJumpTable(count - 1);
            Vector<int32_t, 16> list { table.m_min, static_cast<int32_t>(table.m_branchOffsets.size()) };
            for (int32_t offset : table.m_branchOffsets)
                list.append(offset ? offset : table.m_defaultOffset);
            words[0] |= Metadata::ResumePoints;
            words.append(append(list));
        }
        if (size_t count = codeBlock->numberOfConstantIdentifierSets()) {
            Vector<uint32_t> list { static_cast<uint32_t>(count) };
            for (auto& set : codeBlock->constantIdentifierSets()) {
                Vector<UniquedStringImpl*> inOrder;
                for (auto& name : set)
                    inOrder.append(name.get());
                std::ranges::sort(inOrder, [](auto* a, auto* b) { return codePointCompare(StringView { *a }, StringView { *b }) < 0; });
                Vector<uint32_t> names;
                for (auto* name : inOrder)
                    names.append(numberOfIdentifier(name));
                std::ranges::sort(names);
                list.append(names.size());
                list.appendVector(names);
            }
            words[0] |= Metadata::ConstantIdentifierSets;
            words.append(append(list));
        }
        words[0] |= Metadata::Scalars;
        words.append(appendOnce(encodeCodeBlockScalars(*codeBlock).span(), 1));
        m_functionMetadataOffsets[index] = append(words);
    }

    static constexpr uint32_t invalidExecutableIndex = std::numeric_limits<uint32_t>::max();

    void fillInfo(const ImageView::Function& function, UnlinkedCodeBlock* codeBlock, ScriptExecutable* executable, uint32_t executableIndex, CodeSpecializationKind kind, LineStartTable& lineStarts)
    {
        FunctionInfo& info = m_infos[function.index];
        auto& constantIndices = m_reportableSites[function.index].constantIndices;
        {
            RELEASE_ASSERT(constantIndices.size() == codeBlock->constantRegisters().size());
            for (unsigned i = 0; i < constantIndices.size(); ++i) {
                JSValue value = codeBlock->constantRegisters()[i].get();
                uint32_t constantIndex = constantIndices[i];
                if (codeBlock->constantsSourceCodeRepresentation()[i] == SourceCodeRepresentation::LinkTimeConstant)
                    value = JSValue();
                RELEASE_ASSERT(!value == (constantIndex == invalidConstantIndex));
                if (!value)
                    continue;
                JSValue& inTable = m_constants[constantIndex];
                if (!inTable)
                    inTable = value;
                else if (inTable != value) {
                    if (value.isString()) {
                        RELEASE_ASSERT(inTable.isString());
                        String said = asString(value)->tryGetValue();
                        String otherString = asString(inTable)->tryGetValue();
                        RELEASE_ASSERT(said == otherString);
                    } else
                        RELEASE_ASSERT(uncheckedDowncast<JSTemplateObjectDescriptor>(value.asCell())->descriptor() == uncheckedDowncast<JSTemplateObjectDescriptor>(inTable.asCell())->descriptor());
                }
            }
        }
        auto& numbers = m_reportableSites[function.index].identifierIndices;
        RELEASE_ASSERT(numbers.size() == codeBlock->numberOfIdentifiers());
        for (unsigned i = 0; i < numbers.size(); ++i) {
            UniquedStringImpl*& inTable = m_identifiers[numbers[i]];
            UniquedStringImpl* name = codeBlock->identifier(i).impl();
            RELEASE_ASSERT(!inTable || inTable == name);
            if (!inTable)
                m_identifierIndices.add(name, numbers[i]);
            inTable = name;
        }
        info.sites = safeCast<uint32_t>(std::bit_cast<uintptr_t>(function.sites));
        info.set(executableIndex == invalidExecutableIndex ? 0 : executableIndex + 1, kind, FunctionCode);
        info.flags = (function.hasSiteConstants ? FunctionInfo::hasSiteConstants : FunctionInfo::sitesHaveInlineConstants) | (function.startsCold && executable ? FunctionInfo::startsCold : 0) | FunctionInfo::encodeSlotCountInFlags(function.numSlots);
        fillMetadata(function.index, codeBlock, executable, lineStarts);
    }

    void fillTopLevelCodeInfo(const ImageView::Function& function, UnlinkedCodeBlock* codeBlock, uint32_t numberOfTopLevelCode, LineStartTable& lineStarts)
    {
        fillInfo(function, codeBlock, nullptr, invalidExecutableIndex, CodeSpecializationKind::CodeForCall, lineStarts);
        FunctionInfo& info = m_infos[function.index];
        info.set(numberOfTopLevelCode + 1, CodeSpecializationKind::CodeForCall, codeBlock->codeType());
    }

    void makeExecutables(UnlinkedCodeBlock* codeBlock, const SourceCode& source, bool isInsideOrdinaryFunction, uint32_t moduleIndex, LineStartTable& lineStarts, UnlinkedFunctionExecutable* only = nullptr)
    {
        uint32_t moduleID = m_moduleEntryOffset + 1;
        auto make = [&](UnlinkedFunctionExecutable* unlinked) {
            if (m_unlinkedFunctionExecutable.contains(unlinked))
                return;
            bool isDefaultConstructor = unlinked->isBuiltinDefaultClassConstructor();
            auto functionKey = orderFunctionKey(*unlinked, isDefaultConstructor ? source : unlinked->linkedSourceCode(source));
            if (!functionKey)
                return;
            std::optional<ImageView::Function> code[2];
            for (auto kind : { CodeSpecializationKind::CodeForCall, CodeSpecializationKind::CodeForConstruct }) {
                if (!unlinked->codeBlockIfExists(kind))
                    continue;
                ImageKey key;
                key.module = moduleID;
                key.start = functionKey->start;
                key.kind = static_cast<uint32_t>(functionKey->kind) << 1 | (kind == CodeSpecializationKind::CodeForConstruct);
                code[static_cast<unsigned>(kind)] = m_image.find(key);
            }
            if (!code[0] && !code[1])
                return;
            uint32_t existing = invalidExecutableIndex;
            for (auto& function : code) {
                if (function && m_infos[function->index].indexPlusOne())
                    existing = m_infos[function->index].indexPlusOne() - 1;
            }
            if (existing != invalidExecutableIndex) {
                bool isComplete = true;
                for (auto kind : { CodeSpecializationKind::CodeForCall, CodeSpecializationKind::CodeForConstruct }) {
                    if (auto& function = code[static_cast<unsigned>(kind)])
                        isComplete &= m_infos[function->index].indexPlusOne() == existing + 1 && m_infos[function->index].kind() == kind;
                }
                if (!isComplete)
                    return;
                m_unlinkedFunctionExecutable.add(unlinked, existing);
                FunctionExecutable* executable = m_executables[existing].executable;
                for (auto kind : { CodeSpecializationKind::CodeForCall, CodeSpecializationKind::CodeForConstruct }) {
                    if (auto* nested = unlinked->codeBlockIfExists(kind))
                        makeExecutables(nested, executable->source(), executable->isInsideOrdinaryFunction(), moduleIndex, lineStarts);
                }
                return;
            }
            FunctionExecutable* executable = unlinked->link(m_vm, nullptr, source, std::nullopt, NoIntrinsic, isInsideOrdinaryFunction);
            uint32_t executableIndex = safeCast<uint32_t>(m_executables.size());
            m_executables.append({ executable, moduleIndex });
            for (auto kind : { CodeSpecializationKind::CodeForCall, CodeSpecializationKind::CodeForConstruct }) {
                if (auto& function = code[static_cast<unsigned>(kind)]) {
                    executable->setAOTCode(kind, function->entry, function->index);
                    fillInfo(*function, unlinked->codeBlockIfExists(kind), executable, executableIndex, kind, lineStarts);
                }
            }
            if (code[0] && !code[1] && unlinked->constructAbility() == ConstructAbility::CanConstruct && !unlinked->isClassConstructorFunction())
                executable->setAOTCode(CodeSpecializationKind::CodeForConstruct, m_image.stubCodeOffset(Stub::ConstructViaCall), FunctionExecutable::aotConstructViaCallIndex);
            m_unlinkedFunctionExecutable.add(unlinked, executableIndex);
            for (auto kind : { CodeSpecializationKind::CodeForCall, CodeSpecializationKind::CodeForConstruct }) {
                if (auto* nested = unlinked->codeBlockIfExists(kind))
                    makeExecutables(nested, executable->source(), executable->isInsideOrdinaryFunction(), moduleIndex, lineStarts);
            }
        };
        if (only) {
            make(only);
            return;
        }
        for (unsigned i = 0; i < codeBlock->numberOfFunctionDecls(); ++i)
            make(codeBlock->functionDecl(i));
        for (unsigned i = 0; i < codeBlock->numberOfFunctionExprs(); ++i)
            make(codeBlock->functionExpr(i));
    }

    void dropUnreferencedVariableNames(UnlinkedCodeBlock* codeBlock, DynamicallyResolvedNames& lookedUp, const Vector<uint32_t>* exportedByModule = nullptr)
    {
        DynamicallyResolvedNames own;
        for (const auto& instruction : codeBlock->instructions()) {
            switch (instruction->opcodeID()) {
            case op_call_direct_eval:
                own.mayBeAnything = true;
                break;
            case op_resolve_scope:
                if (auto bytecode = instruction->as<OpResolveScope>(); isLookedUpByName(bytecode.m_resolveType))
                    own.names.add(codeBlock->identifier(bytecode.m_var).impl());
                break;
            case op_get_from_scope:
                if (auto bytecode = instruction->as<OpGetFromScope>(); isLookedUpByName(bytecode.m_getPutInfo.resolveType()))
                    own.names.add(codeBlock->identifier(bytecode.m_var).impl());
                break;
            case op_put_to_scope:
                if (auto bytecode = instruction->as<OpPutToScope>(); isLookedUpByName(bytecode.m_getPutInfo.resolveType()))
                    own.names.add(codeBlock->identifier(bytecode.m_var).impl());
                break;
            default:
                break;
            }
        }
        auto inside = [&](UnlinkedFunctionExecutable* function) {
            if (function->features() & EvalFeature)
                own.mayBeAnything = true;
            bool hasCode = false;
            for (auto kind : { CodeSpecializationKind::CodeForCall, CodeSpecializationKind::CodeForConstruct }) {
                if (auto* nested = function->codeBlockIfExists(kind)) {
                    hasCode = true;
                    dropUnreferencedVariableNames(nested, own);
                }
            }
            if (!hasCode)
                own.mayBeAnything = true;
        };
        for (unsigned i = 0; i < codeBlock->numberOfFunctionDecls(); ++i)
            inside(codeBlock->functionDecl(i));
        for (unsigned i = 0; i < codeBlock->numberOfFunctionExprs(); ++i)
            inside(codeBlock->functionExpr(i));

        SymbolTable* moduleValue = nullptr;
        if (auto* moduleCode = dynamicDowncast<UnlinkedModuleProgramCodeBlock>(codeBlock))
            moduleValue = dynamicDowncast<SymbolTable>(moduleCode->constantRegister(VirtualRegister(moduleCode->moduleEnvironmentSymbolTableConstantRegisterOffset())).get());
        if (moduleValue && exportedByModule && !own.mayBeAnything) {
            UncheckedKeyHashSet<uint32_t, WTF::IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> exported;
            for (uint32_t offset : *exportedByModule)
                exported.add(offset);
            moduleValue->keepOnly([&](UniquedStringImpl* name, const SymbolTableEntry& entry) {
                return name->isSymbol() || own.names.contains(name) || (entry.varOffset().isScope() && exported.contains(entry.scopeOffset().offset()));
            });
        }
        for (auto& constant : codeBlock->constantRegisters()) {
            auto* table = constant.get().isCell() ? dynamicDowncast<SymbolTable>(constant.get().asCell()) : nullptr;
            if (!table || table == moduleValue)
                continue;
            RELEASE_ASSERT(m_processedSymbolTables.add(table).isNewEntry);
            if (!own.mayBeAnything)
                table->keepOnlyNames(own.names);
        }
        lookedUp.mayBeAnything |= own.mayBeAnything;
        for (auto* name : own.names)
            lookedUp.names.add(name);
    }

    void mergeSymbolTables()
    {
        UncheckedKeyHashMap<uint64_t, SymbolTable*, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> kept;
        for (JSValue& constant : m_constants) {
            auto* table = constant && constant.isCell() ? dynamicDowncast<SymbolTable>(constant.asCell()) : nullptr;
            if (!table)
                continue;
            if (auto said = table->namelessContentKey())
                constant = kept.add(*said, table).iterator->value;
        }
    }

    uint32_t numberOfUnlinkedFunction(UnlinkedFunctionExecutable* function)
    {
        return m_unlinkedFunctionIndices.ensure(function, [&] {
            function->clearCodegenOnlyData();
            m_unlinkedFunctions.append(function);
            return safeCast<uint32_t>(m_unlinkedFunctions.size() - 1);
        }).iterator->value;
    }

    uint32_t functionListEntryFor(UnlinkedFunctionExecutable* function)
    {
        if (!function)
            return 0;
        if (auto it = m_unlinkedFunctionExecutable.find(function); it != m_unlinkedFunctionExecutable.end())
            return FunctionMetadata::executableListEntry(it->value);
        return FunctionMetadata::unlinkedFunctionListEntry(numberOfUnlinkedFunction(function));
    }

    void makeRows()
    {
        UncheckedKeyHashMap<String, uint32_t> shared;
        for (auto& [executable, moduleIndex] : m_executables) {
            UnlinkedFunctionExecutable* unlinked = executable->unlinkedExecutable();
            unlinked->clearCodegenOnlyData();
            bool hasCodeToCall = executable->aotEntryFor(CodeSpecializationKind::CodeForCall);
            bool hasCode = hasCodeToCall || (executable->aotEntryFor(CodeSpecializationKind::CodeForConstruct) && !executable->constructsViaCall());
            uint32_t index = executable->aotIndexFor(hasCodeToCall ? CodeSpecializationKind::CodeForCall : CodeSpecializationKind::CodeForConstruct);
            auto hasStartPosition = [&] {
                uint32_t at = m_functionMetadataOffsets[index];
                return at && !(at & 1) && reinterpret_cast<const FunctionMetadata*>(m_out.span().data() + at)->find(FunctionMetadata::ExpressionInfo);
            };
            RELEASE_ASSERT(moduleIndex < (1u << ExecutableRow::moduleBits));
            ExecutableRow row { };
            row.isShort = hasCode && unlinked->canUseSharedTemplate() && hasStartPosition()
                && unlinked->parameterCount() < (1u << ExecutableRow::parameterCountBits)
                && executable->intrinsic() == NoIntrinsic && executable->evalContextType() == EvalContextType::None && !executable->overrideLineNumber()
                && executable->derivedContextType() == unlinked->derivedContextType() && executable->lexicallyScopedFeatures() == unlinked->lexicallyScopedFeatures();
            for (auto kind : { CodeSpecializationKind::CodeForCall, CodeSpecializationKind::CodeForConstruct }) {
                row.entry[static_cast<unsigned>(kind)] = executable->aotEntryFor(kind);
                row.index[static_cast<unsigned>(kind)] = executable->aotIndexFor(kind);
            }
            if (SourceParseMode parseMode = executable->parseMode(); row.isShort && hasCodeToCall && !executable->isBuiltinFunction() && !executable->isClassConstructorFunction()
                && !isGeneratorWrapperParseMode(parseMode) && !isAsyncFunctionWrapperParseMode(parseMode) && !isAsyncGeneratorWrapperParseMode(parseMode)) {
                FunctionStructureKind kind;
                if (executable->isArrowFunction())
                    kind = FunctionStructureKind::Arrow;
                else if (executable->isInStrictContext())
                    kind = executable->hasPrototypeProperty() ? FunctionStructureKind::StrictFunction : FunctionStructureKind::StrictMethod;
                else
                    kind = executable->hasPrototypeProperty() ? FunctionStructureKind::SloppyFunction : FunctionStructureKind::SloppyMethod;
                row.functionStructureKind = static_cast<uint32_t>(kind);
            }
            row.module = moduleIndex;
            row.isArrowFunctionContext = executable->isArrowFunctionContext();
            row.isInsideOrdinaryFunction = executable->isInsideOrdinaryFunction();
            if (row.isShort) {
                row.name = numberOfIdentifier(unlinked->ecmaName().impl());
                row.parameterCount = unlinked->parameterCount();
                auto what = unlinked->sharedTemplateKey();
                row.unlinkedFunction = shared.ensure(String { byteCast<Latin1Character>(asByteSpan(what)) }, [&] {
                    return numberOfUnlinkedFunction(unlinked);
                }).iterator->value;
            } else {
                row.unlinkedFunction = numberOfUnlinkedFunction(unlinked);
                row.startOffset = executable->source().startOffset();
                row.sourceLength = executable->source().length();
            }
            m_rows.append(row);
        }
    }

    VM& m_vm;
    const ImageView& m_image;
    const ProgramData::RetainedPositions& m_positions;
    std::span<const FunctionReportableSites> m_reportableSites;
    uint32_t m_moduleEntryOffset { 0 };

    Vector<uint8_t> m_out;
    UncheckedKeyHashMap<uint64_t, Vector<std::pair<uint32_t, uint32_t>, 1>> m_copies;
    Vector<FunctionInfo> m_infos;
    Vector<uint32_t> m_functionMetadataOffsets;
    Vector<ExecutableRow> m_rows;
    struct Executable {
        FunctionExecutable* executable;
        uint32_t moduleIndex;
    };
    Vector<Executable> m_executables;
    UncheckedKeyHashMap<UnlinkedFunctionExecutable*, uint32_t, DefaultHash<UnlinkedFunctionExecutable*>, HashTraits<UnlinkedFunctionExecutable*>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_unlinkedFunctionExecutable;
    Vector<std::pair<uint32_t, Vector<UnlinkedFunctionExecutable*>>> m_functionLists;
    Vector<UniquedStringImpl*> m_identifiers;
    Vector<RefPtr<AtomStringImpl>> m_switchAtoms;
    UncheckedKeyHashMap<UniquedStringImpl*, uint32_t> m_identifierIndices;
    Vector<JSValue> m_constants;
    UncheckedKeyHashMap<EncodedJSValue, uint32_t, DefaultHash<EncodedJSValue>, WTF::UnsignedWithZeroKeyHashTraits<EncodedJSValue>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_otherConstantIndices;
    Vector<UnlinkedFunctionExecutable*> m_unlinkedFunctions;
    UncheckedKeyHashMap<UnlinkedFunctionExecutable*, uint32_t, DefaultHash<UnlinkedFunctionExecutable*>, HashTraits<UnlinkedFunctionExecutable*>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_unlinkedFunctionIndices;
    Vector<UnlinkedCodeBlock*> m_topLevelCodes;
    UncheckedKeyHashMap<CString, uint32_t> m_sources;
    Vector<CString> m_sourceNames;
    UncheckedKeyHashSet<SymbolTable*> m_processedSymbolTables;
};

class BuildTimeProvider final : public SourceProvider {
public:
    static Ref<BuildTimeProvider> create() { return adoptRef(*new BuildTimeProvider); }
    unsigned hash() const final { return 0; }
    StringView source() const final { return { }; }

private:
    BuildTimeProvider()
        : SourceProvider(SourceOrigin(), String(), String(), SourceTaintedOrigin::Untainted, TextPosition(), SourceProviderSourceType::Program)
    {
    }
};

} // anonymous namespace

Vector<uint8_t> ProgramData::build(VM& vm, std::span<const uint8_t> strings, std::span<const uint8_t> payload, std::span<const uint32_t> moduleEntryOffsets, std::span<const uint8_t> codeImage, const RetainedPositions& positions, std::span<const FunctionReportableSites> reportableSites, std::span<const std::optional<Vector<uint32_t>>> variablesExportedByModules)
{
    auto image = ImageView::tryCreate(codeImage, nullptr);
    if (!image || payload.empty() || moduleEntryOffsets.empty())
        return { };
    DeferGC deferGC(vm);
    Builder builder(vm, *image, positions, reportableSites);

    DecoderStringTable table(strings);
    EncoderStringTable encoderStringTable;
    for (uint32_t ordinal = 0; ordinal < table.count(); ++ordinal)
        RELEASE_ASSERT(encoderStringTable.ordinalFor(table.atomFor(vm, ordinal).get()) == ordinal);

    Vector<uint32_t> sortedOffsets(moduleEntryOffsets);
    std::ranges::sort(sortedOffsets);
    Vector<ProgramModule> modules;
    Vector<Ref<Decoder>> decoders;
    bool ok = true;
    for (size_t i = 0; i < sortedOffsets.size(); ++i) {
        builder.m_moduleEntryOffset = sortedOffsets[i];
        Ref cachedBytecode = CachedBytecode::create(std::span { const_cast<uint8_t*>(payload.data()), payload.size() }, nullptr, { });
        cachedBytecode->setPayloadIsPersistent();
        cachedBytecode->setEntryOffset(sortedOffsets[i]);
        Ref decoder = Decoder::createForProgramData(vm, WTF::move(cachedBytecode), Decoder::IsBuilding::Yes);
        decoders.append(decoder.copyRef());
        decoder->setExternalStrings(table);
        Ref provider = BuildTimeProvider::create();

        SourceCodeKey key;
        Vector<UnlinkedFunctionExecutable*> functions;
        LineStartTable lineStarts;
        auto setLineStarts = [&](const LineStarts& codeLineStarts) {
            lineStarts.setLineStarts(codeLineStarts ? LineStarts { codeLineStarts } : LineStartTable::encode(Vector<unsigned> { 0 }));
        };
        if (entryIsBuiltinFunction(decoder.get())) {
            unsigned builtinLength = 0;
            unsigned builtinStamp = 0;
            LineStarts builtinLineStarts;
            UnlinkedFunctionExecutable* builtinFunction = decodeBuiltinForProgramData(decoder.get(), builtinLength, builtinStamp, builtinLineStarts, functions);
            ProgramModule module { };
            if (builtinFunction) {
                setLineStarts(builtinLineStarts);
                builder.makeExecutables(nullptr, SourceCode { provider.copyRef(), 0, static_cast<int>(builtinLength) }, false, i, lineStarts, builtinFunction);
                DynamicallyResolvedNames lookedUp;
                for (auto kind : { CodeSpecializationKind::CodeForCall, CodeSpecializationKind::CodeForConstruct }) {
                    if (auto* code = builtinFunction->codeBlockIfExists(kind))
                        builder.dropUnreferencedVariableNames(code, lookedUp);
                }
                if (auto it = builder.m_unlinkedFunctionExecutable.find(builtinFunction); it != builder.m_unlinkedFunctionExecutable.end())
                    module.executableIndex = it->value;
            }
            module.entryOffset = sortedOffsets[i];
            module.keyHash = builtinStamp;
            module.keyLength = builtinLength;
            module.isBuiltinFunction = true;
            modules.append(module);
            continue;
        }
        UnlinkedCodeBlock* codeBlock = decodeModuleForProgramData(decoder.get(), key, functions);
        if (codeBlock && (!key.name().isEmpty() || key.functionConstructorParametersEndPosition() != -1))
            codeBlock = nullptr;
        if (!codeBlock) {
            dataLogLn("AOT: the code of a module could not be decoded");
            ok = false;
            continue;
        }
        setLineStarts(uncheckedDowncast<UnlinkedGlobalCodeBlock>(codeBlock)->lineStarts());
        builder.makeExecutables(codeBlock, SourceCode { provider.copyRef(), 0, static_cast<int>(key.length()) }, false, i, lineStarts);
        ProgramModule module { };
        module.entryOffset = sortedOffsets[i];
        module.keyHash = key.hash();
        module.keyLength = static_cast<uint32_t>(key.length());
        module.keyFlags = key.flagsBitsWithoutCodeGenerationMode();
        auto function = image->find(imageKeyForTopLevelCode(sortedOffsets[i] + 1));
        auto* moduleValue = dynamicDowncast<UnlinkedModuleProgramCodeBlock>(codeBlock);
        if (!moduleValue) {
            module.topLevelCodeIndex = safeCast<uint32_t>(builder.m_topLevelCodes.size());
            builder.m_topLevelCodes.append(codeBlock);
        }
        if (function)
            builder.fillTopLevelCodeInfo(*function, codeBlock, safeCast<uint32_t>(modules.size()), lineStarts);
        if (function && moduleValue) {
            module.functionIndex = function->index;
            module.environmentSymbolTable = builder.m_reportableSites[function->index].constantIndices[VirtualRegister(moduleValue->moduleEnvironmentSymbolTableConstantRegisterOffset()).toConstantIndex()];
            RELEASE_ASSERT(module.environmentSymbolTable != invalidConstantIndex);
            module.firstVarScopeOffset = moduleValue->firstVarScopeOffset();
            module.numberOfVarScopeOffsets = moduleValue->numberOfVarScopeOffsets();
            if (auto* slots = moduleValue->heapAllocatedFunctionDeclSlots()) {
                RELEASE_ASSERT(slots->size() == moduleValue->numberOfHeapAllocatedFunctionDecls());
                module.functionDeclarationSlotsOffset = builder.append(slots->offsets().span());
                module.numberOfFunctionDeclarationSlots = slots->size();
            } else
                RELEASE_ASSERT(!moduleValue->numberOfHeapAllocatedFunctionDecls());
            module.features = moduleValue->codeFeatures();
            module.lexicallyScopedFeaturesAndFlags = moduleValue->lexicallyScopedFeatures() | (moduleValue->hasCapturedVariables() ? 1u << 16 : 0);
        }
        uncheckedDowncast<UnlinkedGlobalCodeBlock>(codeBlock)->setLineStarts({ });
        DynamicallyResolvedNames lookedUp;
        const Vector<uint32_t>* exported = nullptr;
        for (size_t index = 0; index < variablesExportedByModules.size() && index < moduleEntryOffsets.size(); ++index) {
            if (moduleEntryOffsets[index] == sortedOffsets[i] && variablesExportedByModules[index])
                exported = &*variablesExportedByModules[index];
        }
        builder.dropUnreferencedVariableNames(codeBlock, lookedUp, exported);
        modules.append(module);
    }
    if (!ok)
        return { };

    builder.mergeSymbolTables();
    builder.makeRows();
    for (auto& [offset, list] : builder.m_functionLists) {
        for (size_t i = 0; i < list.size(); ++i) {
            uint32_t entry = builder.functionListEntryFor(list[i]);
            memcpy(builder.m_out.mutableSpan().data() + offset + i * sizeof(uint32_t), &entry, sizeof(uint32_t));
        }
    }
    for (auto* function : builder.m_unlinkedFunctions)
        function->discardCode();

    ProgramData header { };
    header.magic = expectedMagic;
    header.stamp = imageStamp();
    header.stringsOffset = builder.append(strings, 16);
    header.stringsSize = safeCast<uint32_t>(strings.size());
    {
        ProgramObjects objects;
        objects.identifiers = builder.m_identifiers.span();
        objects.constants = builder.m_constants.span();
        objects.topLevelCodes = builder.m_topLevelCodes.span();
        objects.functionListEntryFor = [&](UnlinkedFunctionExecutable* function) {
            uint32_t entry = builder.functionListEntryFor(function);
            RELEASE_ASSERT(entry);
            return entry;
        };
        for (auto* codeBlock : builder.m_topLevelCodes) {
            for (auto list : { codeBlock->functionDecls(), codeBlock->functionExprs() }) {
                for (auto& function : list) {
                    if (builder.functionListEntryFor(function.get()) & 1)
                        function->discardCode();
                }
            }
        }
        objects.unlinkedFunctions = builder.m_unlinkedFunctions.span();
        Vector<uint8_t> encoded = encodeProgramObjects(vm, encoderStringTable, objects);
        header.objectsOffset = builder.append(encoded.span(), 16);
        header.objectsSize = safeCast<uint32_t>(encoded.size());
    }
    {
        Vector<uint32_t> records;
        records.fill(0, builder.m_constants.size());
        auto* offsets = reinterpret_cast<const uint32_t*>(strings.data()) + 1;
        for (size_t i = 0; i < builder.m_constants.size(); ++i) {
            JSValue constant = builder.m_constants[i];
            if (!constant || !constant.isString())
                continue;
            String said = asString(constant)->tryGetValue();
            if (said.isEmpty())
                continue;
            uint32_t ordinal = encoderStringTable.ordinalFor(*said.impl());
            RELEASE_ASSERT(ordinal < table.count());
            records[i] = header.stringsOffset + offsets[ordinal];
        }
        header.stringConstantRecordsOffset = builder.append(records);
    }
    header.modulesOffset = builder.append(modules);
    header.numberOfModules = modules.size();
    {
        Vector<uint32_t> engineBuiltinModules;
        for (uint32_t i = 0; i < modules.size(); ++i) {
            if (!modules[i].isBuiltinFunction || !modules[i].hasExecutable() || !BuiltinExecutables::isStamp(modules[i].keyHash))
                continue;
            unsigned which = modules[i].keyHash & 0xffff;
            while (engineBuiltinModules.size() <= which)
                engineBuiltinModules.append(0);
            engineBuiltinModules[which] = i + 1;
        }
        header.engineBuiltinModulesOffset = builder.append(engineBuiltinModules);
        header.numberOfEngineBuiltins = engineBuiltinModules.size();
    }
    header.infosOffset = builder.append(builder.m_infos);
    header.numberOfFunctions = builder.m_infos.size();
    header.functionMetadataTableOffset = builder.append(builder.m_functionMetadataOffsets);
    header.executableRowsOffset = builder.append(builder.m_rows);
    header.numberOfExecutables = builder.m_rows.size();
    header.numberOfIdentifiers = builder.m_identifiers.size();
    header.numberOfConstants = builder.m_constants.size();
    header.numberOfUnlinkedFunctions = builder.m_unlinkedFunctions.size();
    header.numberOfTopLevelCodes = builder.m_topLevelCodes.size();
    {
        Vector<uint32_t> starts;
        Vector<uint8_t> text;
        for (auto& name : builder.m_sourceNames) {
            starts.append(text.size());
            text.append(name.span());
        }
        starts.append(text.size());
        header.sourceNamesOffset = builder.append(starts);
        builder.m_out.appendVector(text);
        header.numberOfSources = builder.m_sourceNames.size();
    }
    if (image->keysAreOmitted()) {
        auto isLookedUp = [&](const ImageKey& key) {
            return key.record && key.kind != std::numeric_limits<uint32_t>::max() && !builder.m_infos[image->functionIndexWith(key)].hasExecutable();
        };
        size_t count = 0;
        for (auto& key : image->keys())
            count += isLookedUp(key);
        size_t capacity = 16;
        while (capacity * 3 < count * 4)
            capacity *= 2;
        Vector<ImageKey> kept;
        kept.grow(capacity);
        zeroSpan(kept.mutableSpan());
        for (auto& key : image->keys()) {
            if (!isLookedUp(key))
                continue;
            size_t bucket = key.hash() & (capacity - 1);
            while (kept[bucket].record)
                bucket = (bucket + 1) & (capacity - 1);
            kept[bucket] = key;
        }
        header.imageKeysOffset = builder.append(kept);
        header.imageKeyCapacity = capacity;
    }
    header.size = safeCast<uint32_t>(builder.m_out.size());
    memcpy(builder.m_out.mutableSpan().data(), &header, sizeof(header));
    if (Options::verboseAOTCompilation()) [[unlikely]]
        dataLogLn("AOT: program data: ", header.size, " bytes: strings ", header.stringsSize, ", objects ", header.objectsSize, "; ", header.numberOfFunctions, " functions, ", header.numberOfExecutables, " executables, ", header.numberOfUnlinkedFunctions, " unlinked functions, ", header.numberOfIdentifiers, " identifiers, ", header.numberOfConstants, " constants, ", header.numberOfSources, " sources");
    return WTF::move(builder.m_out);
}

const ProgramData* ProgramData::tryUse(std::span<const uint8_t> bytes)
{
    if (bytes.size() < sizeof(ProgramData) || std::bit_cast<uintptr_t>(bytes.data()) % 16)
        return nullptr;
    auto* data = reinterpret_cast<const ProgramData*>(bytes.data());
    return data->magic == expectedMagic && data->stamp == imageStamp() && data->size <= bytes.size() ? data : nullptr;
}

std::optional<std::pair<size_t, size_t>> ProgramData::stringTableIn(std::span<const uint8_t> bytes)
{
    if (bytes.size() < sizeof(ProgramData))
        return std::nullopt;
    ProgramData header;
    memcpy(&header, bytes.data(), sizeof(header));
    if (header.magic != expectedMagic || header.size > bytes.size() || header.stringsOffset > header.size || header.stringsSize > header.size - header.stringsOffset)
        return std::nullopt;
    return std::pair { static_cast<size_t>(header.stringsOffset), static_cast<size_t>(header.stringsSize) };
}

std::span<const uint32_t> ProgramData::functionDeclarationListEntries(const ProgramModule& module) const
{
    const uint32_t* list = at<FunctionMetadata>(functionMetadataOffsets()[module.functionIndex])->find(FunctionMetadata::FunctionDecls);
    RELEASE_ASSERT(list && list[1] >= module.numberOfFunctionDeclarationSlots);
    return { at<uint32_t>(list[0]), module.numberOfFunctionDeclarationSlots };
}

const ProgramModule* ProgramData::moduleWithEntryOffset(uint32_t entryOffset) const
{
    auto all = modules();
    size_t index = std::ranges::lower_bound(all, entryOffset, { }, &ProgramModule::entryOffset) - all.begin();
    return index != all.size() && all[index].entryOffset == entryOffset ? &all[index] : nullptr;
}

uint32_t ProgramData::executableIndexForFunction(uint32_t functionIndex) const
{
    const FunctionInfo& info = infos()[functionIndex];
    RELEASE_ASSERT(functionIndex < numberOfFunctions && info.indexPlusOne() && !info.isTopLevelCode());
    return info.indexPlusOne() - 1;
}

const ExecutableRow& ProgramData::executableRowForFunction(uint32_t functionIndex) const
{
    return executableRow(executableIndexForFunction(functionIndex));
}

String ProgramData::sourceName(uint32_t source) const
{
    RELEASE_ASSERT(source && source <= numberOfSources);
    auto* starts = at<uint32_t>(sourceNamesOffset);
    auto* text = reinterpret_cast<const char8_t*>(starts + numberOfSources + 1);
    return String::fromUTF8(std::span { text + starts[source - 1], static_cast<size_t>(starts[source] - starts[source - 1]) });
}

LineColumn ProgramData::functionStartPosition(uint32_t functionIndex) const
{
    RELEASE_ASSERT(functionIndex < numberOfFunctions);
    uint32_t word = functionMetadataOffsets()[functionIndex];
    RELEASE_ASSERT(word && !(word & 1));
    const uint32_t* where = at<FunctionMetadata>(word)->find(FunctionMetadata::ExpressionInfo);
    RELEASE_ASSERT(where);
    const uint8_t* bytes = at<uint8_t>(*where);
    auto readVarint = [&] {
        unsigned value = 0;
        for (unsigned shift = 0;; shift += 7) {
            uint8_t byte = *bytes++;
            value |= static_cast<unsigned>(byte & 0x7f) << shift;
            if (!(byte & 0x80))
                return value;
        }
    };
    unsigned line = readVarint();
    return { line, readVarint() };
}

Vector<uint32_t> ProgramData::moduleEntryOffsets() const
{
    Vector<uint32_t> result;
    for (auto& module : modules()) {
        if (!module.isBuiltinFunction && (module.hasTopLevelCode() || module.isCompiledModule()))
            result.append(module.entryOffset);
    }
    return result;
}

std::span<const ImageKey> ProgramData::imageKeys() const
{
    return spanAt<ImageKey>(imageKeysOffset, imageKeyCapacity);
}

struct VMProgram::Impl {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(Impl);

    std::unique_ptr<DecoderStringTable> ownStrings;
    std::unique_ptr<ProgramObjectsDecoder> decoder;
    Vector<JSCell*> createdSinceLastCollection;
    BitVector materializedExecutables;
    Vector<uint32_t> materializedIdentifierCount;
    Vector<UnlinkedFunctionExecutable*> unlinkedFunctions;
    Vector<RefPtr<SourceProvider>> providers;
    UncheckedKeyHashMap<uint32_t, std::unique_ptr<SourceCode>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> shortExecutableSources;
    UncheckedKeyHashMap<uint32_t, FixedVector<UnlinkedStringJumpTable>> stringSwitchJumpTables;
    UncheckedKeyHashMap<uint32_t, FixedVector<IdentifierSet>> identifierSets;
};

template<typename T> static T* zeroedTable(size_t count)
{
    return static_cast<T*>(OSAllocator::reserveAndCommit(roundUpToMultipleOf(WTF::pageSize(), std::max<size_t>(count, 1) * sizeof(T)), OSAllocator::FastMallocPages));
}

template<typename T> static void freeTable(T* table, size_t count)
{
    OSAllocator::decommitAndRelease(table, roundUpToMultipleOf(WTF::pageSize(), std::max<size_t>(count, 1) * sizeof(T)));
}

VMProgram* VMProgram::of(VM& vm)
{
    if (!vm.m_aotProgram && ProgramData::get()) [[unlikely]]
        vm.m_aotProgram = makeUnique<VMProgram>(vm);
    return vm.m_aotProgram.get();
}

VMProgram::VMProgram(VM& vm)
    : m_vm(vm)
    , m_data(*ProgramData::get())
    , m_identifiers(zeroedTable<UniquedStringImpl*>(m_data.numberOfIdentifiers))
    , m_executableChunks(static_cast<FunctionExecutable***>(fastZeroedMalloc((m_data.numberOfExecutables / executablesPerChunk + 1) * sizeof(FunctionExecutable**))))
    , m_impl(makeUnique<Impl>())
{
    m_impl->unlinkedFunctions.fill(nullptr, m_data.numberOfUnlinkedFunctions);
    m_impl->providers.grow(m_data.numberOfModules);
    constexpr uint32_t initialCapacity = 1024;
    m_constantValues = static_cast<EncodedJSValue*>(fastZeroedMalloc(initialCapacity * sizeof(EncodedJSValue)));
    m_constantKeys = static_cast<uint32_t*>(fastZeroedMalloc(initialCapacity * sizeof(uint32_t)));
    m_constantMask = initialCapacity - 1;
    m_impl->materializedExecutables.ensureSize(m_data.numberOfExecutables);
}

VMProgram::~VMProgram()
{
    for (uint32_t index : m_impl->materializedIdentifierCount)
        m_identifiers[index]->deref();
    fastFree(m_constantValues);
    fastFree(m_constantKeys);
    freeTable(m_identifiers, m_data.numberOfIdentifiers);
    for (uint32_t i = 0; i <= m_data.numberOfExecutables / executablesPerChunk; ++i)
        fastFree(m_executableChunks[i]);
    fastFree(m_executableChunks);
}

DecoderStringTable& VMProgram::strings()
{
    if (!m_impl->ownStrings)
        m_impl->ownStrings = makeUnique<DecoderStringTable>(m_data.strings(), DecoderStringTable::Slots::No);
    return *m_impl->ownStrings;
}

static ProgramObjectsDecoder& ensureDecoder(std::unique_ptr<ProgramObjectsDecoder>& decoder, VM& vm, const ProgramData& data, DecoderStringTable& strings)
{
    if (!decoder)
        decoder = makeUnique<ProgramObjectsDecoder>(vm, data.objects(), strings);
    return *decoder;
}

void VMProgram::didMaterialize(JSCell* cell)
{
    m_impl->createdSinceLastCollection.append(cell);
}

JSValue VMProgram::constant(uint32_t index)
{
    RELEASE_ASSERT(index < m_data.numberOfConstants);
    for (uint32_t place = constantHash(index);; ++place) {
        uint32_t key = m_constantKeys[place & m_constantMask];
        if (key == index + 1)
            return JSValue::decode(m_constantValues[place & m_constantMask]);
        if (!key)
            break;
    }
    DeferGC deferGC(m_vm);
    ProgramObjectsDecoder& decoder = ensureDecoder(m_impl->decoder, m_vm, m_data, strings());
    if (auto other = decoder.constantAliasTarget(index)) {
        JSValue value = constant(*other);
        addConstant(index, value);
        return value;
    }
    JSValue value = decoder.constant(index);
    RELEASE_ASSERT(value && (!value.isCell() || !value.isObject()));
    if (value.isCell())
        didMaterialize(value.asCell());
    addConstant(index, value);
    return value;
}

JSValue VMProgram::createTransientConstant(uint32_t index)
{
    RELEASE_ASSERT(index < m_data.numberOfConstants);
    DeferGC deferGC(m_vm);
    ProgramObjectsDecoder& decoder = ensureDecoder(m_impl->decoder, m_vm, m_data, strings());
    if (decoder.constantAliasTarget(index))
        return constant(index);
    SetForScope createsPlainStrings(strings().createsPlainStrings(), true);
    JSValue value = decoder.constant(index);
    RELEASE_ASSERT(value && (!value.isCell() || !value.isObject()));
    return value;
}

void VMProgram::addConstant(uint32_t index, JSValue value)
{
    auto add = [&](uint32_t key, EncodedJSValue encoded) {
        uint32_t place = constantHash(key - 1);
        while (m_constantKeys[place & m_constantMask])
            ++place;
        m_constantValues[place & m_constantMask] = encoded;
        m_constantKeys[place & m_constantMask] = key;
    };
    if (++m_numberOfMaterializedConstants * 2 > m_constantMask) {
        uint32_t oldCapacity = m_constantMask + 1;
        EncodedJSValue* oldValues = m_constantValues;
        uint32_t* oldKeys = m_constantKeys;
        m_constantValues = static_cast<EncodedJSValue*>(fastZeroedMalloc(oldCapacity * 2 * sizeof(EncodedJSValue)));
        m_constantKeys = static_cast<uint32_t*>(fastZeroedMalloc(oldCapacity * 2 * sizeof(uint32_t)));
        m_constantMask = oldCapacity * 2 - 1;
        for (uint32_t i = 0; i < oldCapacity; ++i) {
            if (oldKeys[i])
                add(oldKeys[i], oldValues[i]);
        }
        fastFree(oldValues);
        fastFree(oldKeys);
    }
    add(index + 1, JSValue::encode(value));
}

UniquedStringImpl* VMProgram::identifier(uint32_t index)
{
    RELEASE_ASSERT(index < m_data.numberOfIdentifiers);
    if (UniquedStringImpl* existing = m_identifiers[index])
        return existing;
    Identifier identifier = ensureDecoder(m_impl->decoder, m_vm, m_data, strings()).identifier(index);
    RELEASE_ASSERT(!identifier.isNull());
    identifier.impl()->ref();
    m_impl->materializedIdentifierCount.append(index);
    m_identifiers[index] = identifier.impl();
    return identifier.impl();
}

const Identifier& VMProgram::identifierAsIdentifier(uint32_t index)
{
    static_assert(sizeof(Identifier) == sizeof(UniquedStringImpl*));
    if (index)
        identifier(index);
    return *reinterpret_cast<const Identifier*>(&m_identifiers[index]);
}

UnlinkedFunctionExecutable* VMProgram::unlinkedFunction(uint32_t index, bool isShared)
{
    RELEASE_ASSERT(index < m_data.numberOfUnlinkedFunctions);
    if (UnlinkedFunctionExecutable* existing = m_impl->unlinkedFunctions[index])
        return existing;
    DeferGC deferGC(m_vm);
    UnlinkedFunctionExecutable* result = ensureDecoder(m_impl->decoder, m_vm, m_data, strings()).unlinkedFunction(index);
    RELEASE_ASSERT(result);
    if (isShared)
        result->convertToSharedTemplate();
    didMaterialize(result);
    m_impl->unlinkedFunctions[index] = result;
    return result;
}

UnlinkedCodeBlock* VMProgram::topLevelCode(uint32_t index)
{
    RELEASE_ASSERT(index < m_data.numberOfTopLevelCodes);
    DeferGC deferGC(m_vm);
    UnlinkedCodeBlock* result = ensureDecoder(m_impl->decoder, m_vm, m_data, strings()).topLevelCode(index);
    RELEASE_ASSERT(result);
    return result;
}

UnlinkedCodeBlock* VMProgram::topLevelCodeFor(const SourceCodeKey& key)
{
    SourceProvider& provider = key.source().provider();
    uint32_t id = provider.aotModuleID();
    if (!id || !provider.hasNoSourceText())
        return nullptr;
    const ProgramModule* module = m_data.moduleWithEntryOffset(id - 1);
    if (!module || module->isBuiltinFunction || !module->hasTopLevelCode() || key.flagsBitsWithoutCodeGenerationMode() != module->keyFlags || !key.name().isEmpty() || key.functionConstructorParametersEndPosition() != -1)
        return nullptr;
    didLoadModule(provider);
    return topLevelCode(module->topLevelCodeIndex);
}

const ProgramModule* VMProgram::moduleFor(SourceProvider& provider)
{
    uint32_t id = provider.aotModuleID();
    if (!id || !provider.hasNoSourceText())
        return nullptr;
    const ProgramModule* module = m_data.moduleWithEntryOffset(id - 1);
    if (!module || !module->isCompiledModule())
        return nullptr;
    didLoadModule(provider);
    return module;
}

void VMProgram::didLoadModule(SourceProvider& provider)
{
    const ProgramModule* module = m_data.moduleWithEntryOffset(provider.aotModuleID() - 1);
    RELEASE_ASSERT(module);
    RefPtr<SourceProvider>& first = m_impl->providers[module - m_data.modules().data()];
    if (!first)
        first = &provider;
}

SourceProvider* VMProgram::moduleProvider(uint32_t moduleIndex)
{
    RefPtr<SourceProvider>& provider = m_impl->providers[moduleIndex];
    if (!provider) {
        const ProgramModule& module = m_data.modules()[moduleIndex];
        RELEASE_ASSERT(module.isBuiltinFunction);
        String text;
        if (BuiltinExecutables::isStamp(module.keyHash))
            text = StringImpl::createWithoutCopying(BuiltinExecutables::textOf(module.keyHash & 0xffff));
        provider = StringSourceProvider::create(text, SourceOrigin(), String(), SourceTaintedOrigin::Untainted);
        if (text.isNull())
            provider->setHasNoSourceText();
        provider->setAOTModuleID(module.entryOffset + 1);
    }
    return provider.get();
}

const SourceCode& VMProgram::shortExecutableSource(uint32_t index)
{
    return *m_impl->shortExecutableSources.ensure(index, [&] {
        return makeUniqueWithoutFastMallocCheck<SourceCode>(RefPtr { moduleProvider(m_data.executableRow(index).module) }, 0, 0);
    }).iterator->value;
}

FunctionExecutable* VMProgram::executable(uint32_t executableIndex)
{
    RELEASE_ASSERT(executableIndex < m_data.numberOfExecutables);
    if (FunctionExecutable* existing = executableIfExists(executableIndex))
        return existing;
    RELEASE_ASSERT(!m_vm.heap.isShuttingDown() && m_vm.heap.mutatorState() == MutatorState::Running && !m_vm.heap.worldIsStopped() && !m_vm.heap.objectSpace().isIterating());
    const ExecutableRow& row = m_data.executableRow(executableIndex);
    DeferGC deferGC(m_vm);
    FunctionExecutable* result;
    if (row.isShort)
        result = FunctionExecutable::createInShortForm(m_vm, row.entry, row.index);
    else {
        SourceCode source { RefPtr { moduleProvider(row.module) }, static_cast<int>(row.startOffset), static_cast<int>(row.startOffset + row.sourceLength) };
        result = FunctionExecutable::create(m_vm, nullptr, source, unlinkedFunction(row.unlinkedFunction, false), NoIntrinsic, row.isInsideOrdinaryFunction);
        result->becomeSharedAcrossRealms(m_vm);
        for (auto kind : { CodeSpecializationKind::CodeForCall, CodeSpecializationKind::CodeForConstruct }) {
            if (uint64_t entry = row.entry[static_cast<unsigned>(kind)])
                result->setAOTCode(kind, entry, row.index[static_cast<unsigned>(kind)]);
        }
    }
    didMaterialize(result);
    m_impl->materializedExecutables.quickSet(executableIndex);
    FunctionExecutable**& chunk = m_executableChunks[executableIndex / executablesPerChunk];
    if (!chunk)
        chunk = static_cast<FunctionExecutable**>(fastZeroedMalloc(executablesPerChunk * sizeof(FunctionExecutable*)));
    chunk[executableIndex % executablesPerChunk] = result;
    return result;
}

FunctionExecutable* VMProgram::builtinFunctionFor(uint32_t entryOffset, unsigned embedderStamp, const String& text, const SourceOrigin& sourceOrigin, const String& sourceURL)
{
    if (BytecodeOrderRecorder::ofVM(m_vm))
        return nullptr;
    const ProgramModule* module = m_data.moduleWithEntryOffset(entryOffset);
    if (!module || !module->isBuiltinFunction || !module->hasExecutable() || module->keyHash != embedderStamp || module->keyLength != text.length())
        return nullptr;
    RefPtr<SourceProvider>& provider = m_impl->providers[module - m_data.modules().data()];
    if (!provider) {
        provider = StringSourceProvider::create(text, sourceOrigin, String { sourceURL }, SourceTaintedOrigin::Untainted);
        provider->setAOTModuleID(entryOffset + 1);
    }
    return executable(module->executableIndex);
}

FunctionExecutable* VMProgram::engineBuiltinFor(unsigned index, std::span<const Latin1Character> text)
{
    uint32_t moduleIndexPlusOne = index < m_data.numberOfEngineBuiltins ? m_data.at<uint32_t>(m_data.engineBuiltinModulesOffset)[index] : 0;
    if (!moduleIndexPlusOne)
        return nullptr;
    const ProgramModule& module = m_data.modules()[moduleIndexPlusOne - 1];
    if (BytecodeOrderRecorder::ofVM(m_vm) || module.keyLength != text.size())
        return nullptr;
    RefPtr<SourceProvider>& provider = m_impl->providers[moduleIndexPlusOne - 1];
    if (!provider) {
        provider = StringSourceProvider::create(StringImpl::createWithoutCopying(text), SourceOrigin(), String(), SourceTaintedOrigin::Untainted);
        provider->setAOTModuleID(module.entryOffset + 1);
    }
    return executable(module.executableIndex);
}

const UnlinkedStringJumpTable& VMProgram::stringSwitchJumpTable(uint32_t offsetOfTables, unsigned which)
{
    return m_impl->stringSwitchJumpTables.ensure(offsetOfTables, [&] {
        const uint32_t* words = m_data.at<uint32_t>(offsetOfTables);
        FixedVector<UnlinkedStringJumpTable> tables(*words++);
        for (auto& table : tables) {
            uint32_t count = *words++;
            table.m_defaultOffset = static_cast<int32_t>(*words++);
            for (uint32_t i = 0; i < count; ++i, words += 2) {
                StringImpl* string = identifier(words[0]);
                table.m_offsetTable.add(string, UnlinkedStringJumpTable::OffsetLocation { static_cast<int32_t>(words[1]), i });
                table.m_minLength = std::min(table.m_minLength, string->length());
                table.m_maxLength = std::max(table.m_maxLength, string->length());
            }
        }
        return tables;
    }).iterator->value[which];
}

const IdentifierSet& VMProgram::identifierSet(uint32_t offsetOfSets, unsigned which)
{
    return m_impl->identifierSets.ensure(offsetOfSets, [&] {
        const uint32_t* words = m_data.at<uint32_t>(offsetOfSets);
        FixedVector<IdentifierSet> sets(*words++);
        for (auto& set : sets) {
            for (uint32_t count = *words++; count--;)
                set.add(identifier(*words++));
        }
        return sets;
    }).iterator->value[which];
}

template<typename Visitor>
void VMProgram::visit(Visitor& visitor, CollectionScope scope)
{
    if (scope == CollectionScope::Eden) {
        for (JSCell* cell : m_impl->createdSinceLastCollection)
            visitor.appendUnbarriered(cell);
        return;
    }
    for (uint32_t i = 0; i <= m_constantMask; ++i) {
        if (JSValue value = JSValue::decode(m_constantValues[i]); m_constantKeys[i] && value.isCell())
            visitor.appendUnbarriered(value.asCell());
    }
    m_impl->materializedExecutables.forEachSetBit([&](size_t index) {
        visitor.appendUnbarriered(executableIfExists(index));
    });
    for (auto* function : m_impl->unlinkedFunctions) {
        if (function)
            visitor.appendUnbarriered(function);
    }
}
template void VMProgram::visit(AbstractSlotVisitor&, CollectionScope);
template void VMProgram::visit(SlotVisitor&, CollectionScope);

void VMProgram::didFinishCollection()
{
    m_impl->createdSinceLastCollection.clear();
    if (m_impl->ownStrings)
        m_impl->ownStrings->removeDeadRecentPlainStrings(m_vm);
    if (Options::verboseAOTCompilation()) [[unlikely]] {
        UncheckedKeyHashMap<const ClassInfo*, std::pair<size_t, size_t>> byClass;
        auto count = [&](JSCell* cell) {
            auto& entry = byClass.add(cell->classInfo(), std::pair<size_t, size_t> { }).iterator->value;
            entry.first++;
            entry.second += cell->cellSize();
        };
        for (uint32_t i = 0; i <= m_constantMask; ++i) {
            if (JSValue value = JSValue::decode(m_constantValues[i]); m_constantKeys[i] && value.isCell())
                count(value.asCell());
        }
        m_impl->materializedExecutables.forEachSetBit([&](size_t index) { count(executableIfExists(index)); });
        for (auto* function : m_impl->unlinkedFunctions) {
            if (function)
                count(function);
        }
        dataLog("AOT: made so far: ", m_impl->materializedIdentifierCount.size(), " of ", m_data.numberOfIdentifiers, " identifiers, ", m_numberOfMaterializedConstants, " of ", m_data.numberOfConstants, " constants, ", m_impl->materializedExecutables.bitCount(), " of ", m_data.numberOfExecutables, " executables:");
        for (auto& [info, entry] : byClass)
            dataLog(" ", info->className, " ", entry.first, " (", entry.second, " bytes)");
        dataLogLn("; ", m_impl->stringSwitchJumpTables.size(), " + ", m_impl->identifierSets.size(), " tables of functions");
    }
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
