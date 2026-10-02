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

WTF_MAKE_TZONE_ALLOCATED_IMPL(ProgramOfVM);

// ---- When the program is built.

namespace {

void appendVarint(Vector<uint8_t>& bytes, uint64_t value)
{
    while (value >= 0x80) {
        bytes.append(static_cast<uint8_t>(value) | 0x80);
        value >>= 7;
    }
    bytes.append(static_cast<uint8_t>(value));
}

// The names in a SymbolTable are only needed to find a variable by name at run time. Compiled code refers to a variable by its
// offset, unless the bytecode generator could not tell which scope declares the name. Code that is evaluated at run time can name
// any variable in scope.
struct DynamicallyResolvedNames {
    bool mayBeAny { false };
    UncheckedKeyHashSet<UniquedStringImpl*> names;
};

bool isLookedUpByName(ResolveType type)
{
    return type != ResolvedClosureVar && type != ResolvedLazyClosureVar && type != ModuleVar && !isStaticClosureVarResolveType(type);
}

class Builder {
public:
    Builder(VM& vm, const ImageView& image, const ProgramData::PositionsToKeep& positions, std::span<const ReportableSitesOfFunction> reportableSites)
        : m_vm(vm)
        , m_image(image)
        , m_positions(positions)
        , m_reportableSites(reportableSites)
    {
        m_out.fill(0, roundUpToMultipleOf<16>(sizeof(ProgramData)));
        m_infos.grow(image.numberOfFunctions());
        zeroSpan(m_infos.mutableSpan());
        m_functionMetadataOffsets.fill(0, image.numberOfFunctions());
        m_identifiers.fill(nullptr, image.numberOfIdentifiersOfProgram());
        m_constants.fill(JSValue(), image.numberOfConstantsOfProgram());
        RELEASE_ASSERT(image.numberOfIdentifiersOfProgram() && reportableSites.size() == image.numberOfFunctions());
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

    // Equal arrays share one copy.
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
        return m_numbersOfIdentifiers.ensure(name, [&] {
            m_identifiers.append(name);
            return safeCast<uint32_t>(m_identifiers.size() - 1);
        }).iterator->value;
    }

    uint32_t numberOfConstant(JSValue value)
    {
        return m_numbersOfOtherConstants.ensure(JSValue::encode(value), [&] {
            m_constants.append(value);
            return safeCast<uint32_t>(m_constants.size() - 1);
        }).iterator->value;
    }

    // The format of a function's positions, as a sequence of varints:
    //   - The line and column where the function starts in the module's text (ProgramData::whereFunctionStarts()).
    //   - The number of entries.
    //   - For each entry, in bytecode order: (delta of the bytecode offset from the previous entry) << 1 | isConstruction, then a
    //     position. A construction has a second position, where its expression starts.
    // A position on the same line of the same source as the previous one is zigzag(column delta) << 1 | 1. Any other position is
    // zigzag(line delta) << 2 | sourceChanged << 1, then the number of the source if it changed (zero: no source, the position is in
    // the module's text), then the column.
    uint32_t makePositions(uint32_t index, UnlinkedCodeBlock* codeBlock, unsigned sourceOffset, LineStartTable& lineStarts)
    {
        Vector<uint8_t> stream;
        auto& sites = m_positions.sites[index];
        Vector<uint32_t> offsets = sites.offsets;
        // Also keep the resume points of an async function, which is where it reports being while it is suspended
        // (FunctionRef::resumePointOf()).
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
            if (m_positions.find(m_entryOffsetOfModule, inModule, name, position)) {
                source = m_sources.ensure(name, [&] {
                    m_namesOfSources.append(name);
                    return static_cast<uint32_t>(m_namesOfSources.size());
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
                    start.column = construction->columnOrColumnsLeft;
                } else if (position.column > construction->columnOrColumnsLeft)
                    start.column = position.column - construction->columnOrColumnsLeft;
                appendPosition(start);
            }
        }
        // (At an even offset: see FunctionMetadata.)
        return append(stream.span(), 2);
    }

    // What the list says of each function is filled in once every executable has its number.
    uint32_t appendListOfFunctions(std::span<const WriteBarrier<UnlinkedFunctionExecutable>> functions)
    {
        Vector<uint32_t> zeros;
        zeros.fill(0, functions.size());
        uint32_t offset = append(zeros);
        Vector<UnlinkedFunctionExecutable*> list;
        for (auto& function : functions)
            list.append(function.get());
        m_listsOfFunctions.append({ offset, WTF::move(list) });
        return offset;
    }

    void fillMetadata(uint32_t index, UnlinkedCodeBlock* codeBlock, ScriptExecutable* executable, LineStartTable& lineStarts)
    {
        if (m_functionMetadataOffsets[index])
            return;
        if (codeBlock->codeType() != FunctionCode) {
            // The code of a module. Its UnlinkedCodeBlock is kept, so only the positions are needed.
            m_functionMetadataOffsets[index] = makePositions(index, codeBlock, 0, lineStarts) | 1;
            return;
        }
        using Metadata = FunctionMetadata;
        RELEASE_ASSERT(codeBlock->instructions().size() < (1u << (32 - Metadata::shiftOfInstructionsSize)));
        Vector<uint32_t, 16> words { static_cast<uint32_t>(codeBlock->instructions().size()) << Metadata::shiftOfInstructionsSize | (codeBlock->isBuiltinFunction() ? Metadata::isBuiltinFunction : 0) };
        // (A default class constructor gets its own executable in every realm, and has no position in any source.)
        if (executable) {
            words[0] |= Metadata::ExpressionInfo;
            words.append(makePositions(index, codeBlock, executable->source().startOffset(), lineStarts));
        }
        if (size_t count = codeBlock->numberOfExceptionHandlers()) {
            // (Field by field, into zeros: what goes into the file has no bits that nothing has set.)
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
            words.append(appendListOfFunctions(all));
            words.append(all.size());
        };
        functions(Metadata::FunctionDecls, codeBlock->functionDecls());
        functions(Metadata::FunctionExprs, codeBlock->functionExprs());
        if (size_t count = codeBlock->numberOfUnlinkedStringSwitchJumpTables()) {
            Vector<uint32_t> list { static_cast<uint32_t>(count) };
            for (size_t i = 0; i < count; ++i) {
                auto& table = codeBlock->unlinkedStringSwitchJumpTable(i);
                // (The order in which the table gives them differs from one run to the next, and they may be given numbers here.)
                Vector<std::pair<StringImpl*, int32_t>> inOrder;
                for (auto& entry : table.m_offsetTable)
                    inOrder.append({ entry.key.get(), entry.value.m_branchOffset });
                std::ranges::sort(inOrder, [](auto& a, auto& b) { return codePointCompare(StringView { *a.first }, StringView { *b.first }) < 0; });
                Vector<std::pair<uint32_t, int32_t>> entries;
                for (auto [string, offset] : inOrder) {
                    m_atomsOfSwitches.append(AtomStringImpl::add(string));
                    entries.append({ numberOfIdentifier(m_atomsOfSwitches.last().get()), offset });
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
        words.append(appendOnce(scalarsToMakeFunctionCodeFrom(*codeBlock).span(), 1));
        m_functionMetadataOffsets[index] = append(words);
    }

    static constexpr uint32_t noExecutable = std::numeric_limits<uint32_t>::max();

    void fillInfo(const ImageView::Function& function, UnlinkedCodeBlock* codeBlock, ScriptExecutable* executable, uint32_t numberOfExecutable, CodeSpecializationKind kind, LineStartTable& lineStarts)
    {
        FunctionInfo& info = m_infos[function.index];
        auto& numbersOfConstants = m_reportableSites[function.index].numbersOfConstants;
        {
            RELEASE_ASSERT(numbersOfConstants.size() == codeBlock->constantRegisters().size());
            for (unsigned i = 0; i < numbersOfConstants.size(); ++i) {
                JSValue value = codeBlock->constantRegisters()[i].get();
                uint32_t number = numbersOfConstants[i];
                // (A link-time constant is not read as a constant: see NodeKind::LinkTimeConstant.)
                if (codeBlock->constantsSourceCodeRepresentation()[i] == SourceCodeRepresentation::LinkTimeConstant)
                    value = JSValue();
                RELEASE_ASSERT(!value == (number == notAConstantOfProgram));
                if (!value)
                    continue;
                JSValue& inTable = m_constants[number];
                if (!inTable)
                    inTable = value;
                else if (inTable != value) {
                    // Two JSStrings with equal contents, or what two pieces of code have for one template. Either one can be used for both.
                    if (value.isString()) {
                        RELEASE_ASSERT(inTable.isString());
                        String said = asString(value)->tryGetValue();
                        String saidByOther = asString(inTable)->tryGetValue();
                        RELEASE_ASSERT(said == saidByOther);
                    } else
                        RELEASE_ASSERT(uncheckedDowncast<JSTemplateObjectDescriptor>(value.asCell())->descriptor() == uncheckedDowncast<JSTemplateObjectDescriptor>(inTable.asCell())->descriptor());
                }
            }
        }
        auto& numbers = m_reportableSites[function.index].numbersOfIdentifiers;
        RELEASE_ASSERT(numbers.size() == codeBlock->numberOfIdentifiers());
        for (unsigned i = 0; i < numbers.size(); ++i) {
            UniquedStringImpl*& inTable = m_identifiers[numbers[i]];
            UniquedStringImpl* name = codeBlock->identifier(i).impl();
            RELEASE_ASSERT(!inTable || inTable == name);
            if (!inTable)
                m_numbersOfIdentifiers.add(name, numbers[i]);
            inTable = name;
        }
        info.sites = safeCast<uint32_t>(std::bit_cast<uintptr_t>(function.sites));
        info.set(numberOfExecutable == noExecutable ? 0 : numberOfExecutable + 1, kind, false);
        info.flags = (function.hasSiteConstants ? FunctionInfo::hasSiteConstants : FunctionInfo::sitesHaveTheirConstants) | (function.startsCold && executable ? FunctionInfo::startsCold : 0) | FunctionInfo::slotsAmongFlags(function.numSlots);
        fillMetadata(function.index, codeBlock, executable, lineStarts);
    }

    void fillInfoOfTopLevelCode(const ImageView::Function& function, UnlinkedCodeBlock* codeBlock, uint32_t numberOfTopLevelCode, LineStartTable& lineStarts)
    {
        fillInfo(function, codeBlock, nullptr, noExecutable, CodeSpecializationKind::CodeForCall, lineStarts);
        FunctionInfo& info = m_infos[function.index];
        info.set(numberOfTopLevelCode + 1, CodeSpecializationKind::CodeForCall, true);
    }

    // Numbers the executables of the functions nested in `codeBlock`, whose source is `source`, recursively.
    void makeExecutables(UnlinkedCodeBlock* codeBlock, const SourceCode& source, bool isInsideOrdinaryFunction, uint32_t indexOfModule, LineStartTable& lineStarts, UnlinkedFunctionExecutable* only = nullptr)
    {
        uint32_t moduleID = m_entryOffsetOfModule + 1;
        auto make = [&](UnlinkedFunctionExecutable* unlinked) {
            if (m_executableOfUnlinkedFunction.contains(unlinked))
                return;
            // The source of a default class constructor belongs to the engine, so its executable is created at run time.
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
            if (isDefaultConstructor) {
                for (auto kind : { CodeSpecializationKind::CodeForCall, CodeSpecializationKind::CodeForConstruct }) {
                    if (auto& function = code[static_cast<unsigned>(kind)]; function && !m_infos[function->index].sites)
                        fillInfo(*function, unlinked->codeBlockIfExists(kind), nullptr, noExecutable, kind, lineStarts);
                }
                return;
            }
            // A function compiled both for call and for construct has two copies of its nested functions. Each nested function still
            // has only one compiled function, which belongs to one executable (FunctionInfo).
            uint32_t existing = noExecutable;
            for (auto& function : code) {
                if (function && m_infos[function->index].oneMoreThanNumber())
                    existing = m_infos[function->index].oneMoreThanNumber() - 1;
            }
            if (existing != noExecutable) {
                bool isComplete = true;
                for (auto kind : { CodeSpecializationKind::CodeForCall, CodeSpecializationKind::CodeForConstruct }) {
                    if (auto& function = code[static_cast<unsigned>(kind)])
                        isComplete &= m_infos[function->index].oneMoreThanNumber() == existing + 1 && m_infos[function->index].kind() == kind;
                }
                if (!isComplete)
                    return;
                m_executableOfUnlinkedFunction.add(unlinked, existing);
                FunctionExecutable* executable = m_executables[existing].executable;
                for (auto kind : { CodeSpecializationKind::CodeForCall, CodeSpecializationKind::CodeForConstruct }) {
                    if (auto* nested = unlinked->codeBlockIfExists(kind))
                        makeExecutables(nested, executable->source(), executable->isInsideOrdinaryFunction(), indexOfModule, lineStarts);
                }
                return;
            }
            FunctionExecutable* executable = unlinked->link(m_vm, nullptr, source, std::nullopt, NoIntrinsic, isInsideOrdinaryFunction);
            uint32_t number = safeCast<uint32_t>(m_executables.size());
            m_executables.append({ executable, indexOfModule });
            for (auto kind : { CodeSpecializationKind::CodeForCall, CodeSpecializationKind::CodeForConstruct }) {
                if (auto& function = code[static_cast<unsigned>(kind)]) {
                    executable->setAOTCode(kind, function->entry, function->index);
                    fillInfo(*function, unlinked->codeBlockIfExists(kind), executable, number, kind, lineStarts);
                }
            }
            // (Any other function either has code for construct or cannot be constructed.)
            if (code[0] && !code[1] && unlinked->constructAbility() == ConstructAbility::CanConstruct && !unlinked->isClassConstructorFunction())
                executable->setAOTCode(CodeSpecializationKind::CodeForConstruct, m_image.offsetInCodeOfStub(Stub::ConstructByCalling), FunctionExecutable::aotIndexOfWhatConstructsByCalling);
            m_executableOfUnlinkedFunction.add(unlinked, number);
            for (auto kind : { CodeSpecializationKind::CodeForCall, CodeSpecializationKind::CodeForConstruct }) {
                if (auto* nested = unlinked->codeBlockIfExists(kind))
                    makeExecutables(nested, executable->source(), executable->isInsideOrdinaryFunction(), indexOfModule, lineStarts);
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

    // Adds the names that the code and its nested functions look up to `lookedUp`, and drops the other names from the symbol tables of
    // the scopes the code creates. No other code can see those scopes.
    // exportedByModule: for the code of a module, the scope offsets of the variables it exports, if known.
    void dropUnreferencedVariableNames(UnlinkedCodeBlock* codeBlock, DynamicallyResolvedNames& lookedUp, const Vector<uint32_t>* exportedByModule = nullptr)
    {
        DynamicallyResolvedNames own;
        for (const auto& instruction : codeBlock->instructions()) {
            switch (instruction->opcodeID()) {
            case op_call_direct_eval:
                own.mayBeAny = true;
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
                own.mayBeAny = true;
            bool hasCode = false;
            for (auto kind : { CodeSpecializationKind::CodeForCall, CodeSpecializationKind::CodeForConstruct }) {
                if (auto* nested = function->codeBlockIfExists(kind)) {
                    hasCode = true;
                    dropUnreferencedVariableNames(nested, own);
                }
            }
            // (Without its code, assume that it may look up any name.)
            if (!hasCode)
                own.mayBeAny = true;
        };
        for (unsigned i = 0; i < codeBlock->numberOfFunctionDecls(); ++i)
            inside(codeBlock->functionDecl(i));
        for (unsigned i = 0; i < codeBlock->numberOfFunctionExprs(); ++i)
            inside(codeBlock->functionExpr(i));

        // A module's environment is also searched by name to resolve the module's exports, and by the engine for its private names.
        SymbolTable* ofModule = nullptr;
        if (auto* moduleCode = dynamicDowncast<UnlinkedModuleProgramCodeBlock>(codeBlock))
            ofModule = dynamicDowncast<SymbolTable>(moduleCode->constantRegister(VirtualRegister(moduleCode->moduleEnvironmentSymbolTableConstantRegisterOffset())).get());
        if (ofModule && exportedByModule && !own.mayBeAny) {
            UncheckedKeyHashSet<uint32_t, WTF::IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> exported;
            for (uint32_t offset : *exportedByModule)
                exported.add(offset);
            ofModule->keepOnly([&](UniquedStringImpl* name, const SymbolTableEntry& entry) {
                return name->isSymbol() || own.names.contains(name) || (entry.varOffset().isScope() && exported.contains(entry.scopeOffset().offset()));
            });
        }
        for (auto& constant : codeBlock->constantRegisters()) {
            auto* table = constant.get().isCell() ? dynamicDowncast<SymbolTable>(constant.get().asCell()) : nullptr;
            if (!table || table == ofModule)
                continue;
            // Which names are kept depends on the code that can see the scope: the code that owns the table and the functions
            // nested in it. So each table must be processed only once.
            RELEASE_ASSERT(m_processedSymbolTables.add(table).isNewEntry);
            if (!own.mayBeAny)
                table->keepOnlyNames(own.names);
        }
        lookedUp.mayBeAny |= own.mayBeAny;
        for (auto* name : own.names)
            lookedUp.names.add(name);
    }

    // By now dropUnreferencedVariableNames() has removed the names that nothing looks up, so most tables only say how many variables
    // there are and what kind of scope it is. Nothing writes to them or compares them by identity.
    void mergeSymbolTables()
    {
        UncheckedKeyHashMap<uint64_t, SymbolTable*, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> kept;
        for (JSValue& constant : m_constants) {
            auto* table = constant && constant.isCell() ? dynamicDowncast<SymbolTable>(constant.asCell()) : nullptr;
            if (!table)
                continue;
            if (auto said = table->whatIsSaidWithoutNames())
                constant = kept.add(*said, table).iterator->value;
        }
    }

    uint32_t numberOfUnlinkedFunction(UnlinkedFunctionExecutable* function)
    {
        return m_numbersOfUnlinkedFunctions.ensure(function, [&] {
            function->dropWhatOnlyGeneratingCodeNeeds();
            m_unlinkedFunctions.append(function);
            return safeCast<uint32_t>(m_unlinkedFunctions.size() - 1);
        }).iterator->value;
    }

    uint32_t entryInListFor(UnlinkedFunctionExecutable* function)
    {
        if (!function)
            return 0;
        if (auto it = m_executableOfUnlinkedFunction.find(function); it != m_executableOfUnlinkedFunction.end())
            return FunctionMetadata::executableInList(it->value);
        return FunctionMetadata::unlinkedFunctionInList(numberOfUnlinkedFunction(function));
    }

    // Most of a FunctionExecutable and of its UnlinkedFunctionExecutable is for code that has yet to be parsed, compiled or replaced,
    // and a program that ships without its bytecode has none. So most functions get the short form of the one (see
    // FunctionExecutable::sizeOfShortForm), and share the other with every function that only differs in what the row says.
    void makeRows()
    {
        UncheckedKeyHashMap<String, uint32_t> shared;
        for (auto& [executable, indexOfModule] : m_executables) {
            UnlinkedFunctionExecutable* unlinked = executable->unlinkedExecutable();
            unlinked->dropWhatOnlyGeneratingCodeNeeds();
            bool hasCodeToCall = executable->aotEntryFor(CodeSpecializationKind::CodeForCall);
            bool hasCode = hasCodeToCall || (executable->aotEntryFor(CodeSpecializationKind::CodeForConstruct) && !executable->constructsByCalling());
            uint32_t index = executable->aotIndexFor(hasCodeToCall ? CodeSpecializationKind::CodeForCall : CodeSpecializationKind::CodeForConstruct);
            auto hasStartPosition = [&] {
                uint32_t at = m_functionMetadataOffsets[index];
                return at && !(at & 1) && reinterpret_cast<const FunctionMetadata*>(m_out.span().data() + at)->find(FunctionMetadata::ExpressionInfo);
            };
            RELEASE_ASSERT(indexOfModule < (1u << RowOfExecutable::bitsOfModule));
            RowOfExecutable row { };
            row.isShort = hasCode && unlinked->canBeSharedByShortExecutables() && hasStartPosition()
                && unlinked->parameterCount() < (1u << RowOfExecutable::bitsOfParameterCount)
                && executable->intrinsic() == NoIntrinsic && executable->evalContextType() == EvalContextType::None && !executable->overrideLineNumber()
                && executable->derivedContextType() == unlinked->derivedContextType() && executable->lexicallyScopedFeatures() == unlinked->lexicallyScopedFeatures();
            for (auto kind : { CodeSpecializationKind::CodeForCall, CodeSpecializationKind::CodeForConstruct }) {
                row.entry[static_cast<unsigned>(kind)] = executable->aotEntryFor(kind);
                row.index[static_cast<unsigned>(kind)] = executable->aotIndexFor(kind);
            }
            row.module = indexOfModule;
            row.isArrowFunctionContext = executable->isArrowFunctionContext();
            row.isInsideOrdinaryFunction = executable->isInsideOrdinaryFunction();
            if (row.isShort) {
                row.name = numberOfIdentifier(unlinked->ecmaName().impl());
                row.parameterCount = unlinked->parameterCount();
                auto what = unlinked->whatIsSharedByShortExecutables();
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
    const ProgramData::PositionsToKeep& m_positions;
    std::span<const ReportableSitesOfFunction> m_reportableSites;
    uint32_t m_entryOffsetOfModule { 0 };

    Vector<uint8_t> m_out;
    UncheckedKeyHashMap<uint64_t, Vector<std::pair<uint32_t, uint32_t>, 1>> m_copies;
    Vector<FunctionInfo> m_infos;
    Vector<uint32_t> m_functionMetadataOffsets;
    Vector<RowOfExecutable> m_rows;
    struct Executable {
        FunctionExecutable* executable;
        uint32_t indexOfModule;
    };
    Vector<Executable> m_executables;
    UncheckedKeyHashMap<UnlinkedFunctionExecutable*, uint32_t, DefaultHash<UnlinkedFunctionExecutable*>, HashTraits<UnlinkedFunctionExecutable*>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_executableOfUnlinkedFunction;
    Vector<std::pair<uint32_t, Vector<UnlinkedFunctionExecutable*>>> m_listsOfFunctions;
    Vector<UniquedStringImpl*> m_identifiers;
    Vector<RefPtr<AtomStringImpl>> m_atomsOfSwitches; // (Nothing else may refer to them.)
    UncheckedKeyHashMap<UniquedStringImpl*, uint32_t> m_numbersOfIdentifiers;
    Vector<JSValue> m_constants;
    UncheckedKeyHashMap<EncodedJSValue, uint32_t, DefaultHash<EncodedJSValue>, WTF::UnsignedWithZeroKeyHashTraits<EncodedJSValue>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_numbersOfOtherConstants;
    Vector<UnlinkedFunctionExecutable*> m_unlinkedFunctions;
    UncheckedKeyHashMap<UnlinkedFunctionExecutable*, uint32_t, DefaultHash<UnlinkedFunctionExecutable*>, HashTraits<UnlinkedFunctionExecutable*>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_numbersOfUnlinkedFunctions;
    Vector<UnlinkedCodeBlock*> m_topLevelCodes;
    UncheckedKeyHashMap<CString, uint32_t> m_sources;
    Vector<CString> m_namesOfSources;
    UncheckedKeyHashSet<SymbolTable*> m_processedSymbolTables;
};

// What the executables that are made while a program is built say their source is. Nothing reads it.
class ProviderWhileBuilding final : public SourceProvider {
public:
    static Ref<ProviderWhileBuilding> create() { return adoptRef(*new ProviderWhileBuilding); }
    unsigned hash() const final { return 0; }
    StringView source() const final { return { }; }

private:
    ProviderWhileBuilding()
        : SourceProvider(SourceOrigin(), String(), String(), SourceTaintedOrigin::Untainted, TextPosition(), SourceProviderSourceType::Program)
    {
    }
};

} // anonymous namespace

Vector<uint8_t> ProgramData::build(VM& vm, std::span<const uint8_t> strings, std::span<const uint8_t> payload, std::span<const uint32_t> entryOffsetsOfModules, std::span<const uint8_t> imageOfCode, const PositionsToKeep& positions, std::span<const ReportableSitesOfFunction> reportableSites, std::span<const std::optional<Vector<uint32_t>>> variablesExportedByModules)
{
    auto image = ImageView::tryCreate(imageOfCode, nullptr);
    if (!image || payload.empty() || entryOffsetsOfModules.empty())
        return { };
    DeferGC deferGC(vm);
    Builder builder(vm, *image, positions, reportableSites);

    DecoderStringTable table(strings);
    EncoderStringTable stringsToEncodeWith;
    for (uint32_t ordinal = 0; ordinal < table.count(); ++ordinal)
        RELEASE_ASSERT(stringsToEncodeWith.ordinalFor(table.atomFor(vm, ordinal).get()) == ordinal);

    Vector<uint32_t> sortedOffsets(entryOffsetsOfModules);
    std::ranges::sort(sortedOffsets);
    Vector<ModuleOfProgram> modules;
    Vector<Ref<Decoder>> decoders;
    bool ok = true;
    for (size_t i = 0; i < sortedOffsets.size(); ++i) {
        builder.m_entryOffsetOfModule = sortedOffsets[i];
        Ref cachedBytecode = CachedBytecode::create(std::span { const_cast<uint8_t*>(payload.data()), payload.size() }, nullptr, { });
        cachedBytecode->setPayloadIsPersistent();
        cachedBytecode->setEntryOffset(sortedOffsets[i]);
        Ref decoder = Decoder::createForProgramData(vm, WTF::move(cachedBytecode), Decoder::IsBuilding::Yes);
        decoders.append(decoder.copyRef());
        decoder->setExternalStrings(table);
        Ref provider = ProviderWhileBuilding::create();

        SourceCodeKey key;
        Vector<UnlinkedFunctionExecutable*> functions;
        LineStartTable lineStarts;
        auto setLineStarts = [&](const LineStarts& ofCode) {
            // (A short builtin comes without. Its positions are all on its first line.)
            lineStarts.setLineStarts(ofCode ? LineStarts { ofCode } : LineStartTable::encode(Vector<unsigned> { 0 }));
        };
        if (entryIsOfBuiltinFunction(decoder.get())) {
            unsigned lengthOfBuiltin = 0;
            unsigned stampOfBuiltin = 0;
            LineStarts lineStartsOfBuiltin;
            UnlinkedFunctionExecutable* builtinFunction = decodeBuiltinForProgramData(decoder.get(), lengthOfBuiltin, stampOfBuiltin, lineStartsOfBuiltin, functions);
            uint32_t number = 0;
            if (builtinFunction) {
                setLineStarts(lineStartsOfBuiltin);
                builder.makeExecutables(nullptr, SourceCode { provider.copyRef(), 0, static_cast<int>(lengthOfBuiltin) }, false, i, lineStarts, builtinFunction);
                DynamicallyResolvedNames lookedUp;
                for (auto kind : { CodeSpecializationKind::CodeForCall, CodeSpecializationKind::CodeForConstruct }) {
                    if (auto* code = builtinFunction->codeBlockIfExists(kind))
                        builder.dropUnreferencedVariableNames(code, lookedUp);
                }
                if (auto it = builder.m_executableOfUnlinkedFunction.find(builtinFunction); it != builder.m_executableOfUnlinkedFunction.end())
                    number = it->value + 1;
            }
            modules.append({ sortedOffsets[i], stampOfBuiltin, lengthOfBuiltin, 0, true, number });
            continue;
        }
        UnlinkedCodeBlock* codeBlock = decodeAllForProgramData(decoder.get(), key, functions);
        if (codeBlock && (!key.name().isEmpty() || key.functionConstructorParametersEndPosition() != -1))
            codeBlock = nullptr;
        if (!codeBlock) {
            dataLogLn("AOT: the code of a module could not be decoded");
            ok = false;
            continue;
        }
        setLineStarts(uncheckedDowncast<UnlinkedGlobalCodeBlock>(codeBlock)->lineStarts());
        builder.makeExecutables(codeBlock, SourceCode { provider.copyRef(), 0, static_cast<int>(key.length()) }, false, i, lineStarts);
        uint32_t number = safeCast<uint32_t>(builder.m_topLevelCodes.size());
        builder.m_topLevelCodes.append(codeBlock);
        // The top-level code of the module. Its executable is created at run time.
        if (auto function = image->find(imageKeyForTopLevelCode(sortedOffsets[i] + 1)))
            builder.fillInfoOfTopLevelCode(*function, codeBlock, number, lineStarts);
        uncheckedDowncast<UnlinkedGlobalCodeBlock>(codeBlock)->setLineStarts({ });
        // (Its variables are set to undefined by their offsets: CyclicModuleRecord::initializeEnvironment().)
        if (auto* ofModule = dynamicDowncast<UnlinkedModuleProgramCodeBlock>(codeBlock))
            ofModule->setVariableDeclarations({ });
        DynamicallyResolvedNames lookedUp;
        const Vector<uint32_t>* exported = nullptr;
        for (size_t index = 0; index < variablesExportedByModules.size() && index < entryOffsetsOfModules.size(); ++index) {
            if (entryOffsetsOfModules[index] == sortedOffsets[i] && variablesExportedByModules[index])
                exported = &*variablesExportedByModules[index];
        }
        builder.dropUnreferencedVariableNames(codeBlock, lookedUp, exported);
        modules.append({ sortedOffsets[i], key.hash(), static_cast<uint32_t>(key.length()), key.flagsBits(), false, number + 1 });
    }
    if (!ok)
        return { };

    builder.mergeSymbolTables();
    builder.makeRows();
    for (auto& [offset, list] : builder.m_listsOfFunctions) {
        for (size_t i = 0; i < list.size(); ++i) {
            uint32_t entry = builder.entryInListFor(list[i]);
            memcpy(builder.m_out.mutableSpan().data() + offset + i * sizeof(uint32_t), &entry, sizeof(uint32_t));
        }
    }
    // (The code of the functions has been looked at, and none of it is kept.)
    for (auto* function : builder.m_unlinkedFunctions)
        function->leaveWithoutCode();

    ProgramData header { };
    header.magic = expectedMagic;
    header.stamp = imageStamp();
    header.offsetOfStrings = builder.append(strings, 16);
    header.sizeOfStrings = safeCast<uint32_t>(strings.size());
    {
        ObjectsOfProgram objects;
        objects.identifiers = builder.m_identifiers.span();
        objects.constants = builder.m_constants.span();
        objects.topLevelCodes = builder.m_topLevelCodes.span();
        objects.entryInListFor = [&](UnlinkedFunctionExecutable* function) {
            uint32_t entry = builder.entryInListFor(function);
            RELEASE_ASSERT(entry);
            return entry;
        };
        // (Which may add to the unlinked functions.)
        for (auto* codeBlock : builder.m_topLevelCodes) {
            for (auto list : { codeBlock->functionDecls(), codeBlock->functionExprs() }) {
                for (auto& function : list) {
                    if (builder.entryInListFor(function.get()) & 1)
                        function->leaveWithoutCode();
                }
            }
        }
        objects.unlinkedFunctions = builder.m_unlinkedFunctions.span();
        Vector<uint8_t> encoded = encodeObjectsOfProgram(vm, stringsToEncodeWith, objects);
        header.offsetOfObjects = builder.append(encoded.span(), 16);
        header.sizeOfObjects = safeCast<uint32_t>(encoded.size());
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
            uint32_t ordinal = stringsToEncodeWith.ordinalFor(*said.impl());
            RELEASE_ASSERT(ordinal < table.count());
            records[i] = header.offsetOfStrings + offsets[ordinal];
        }
        header.offsetOfRecordsOfStringConstants = builder.append(records);
    }
    header.offsetOfModules = builder.append(modules);
    header.numberOfModules = modules.size();
    {
        Vector<uint32_t> modulesOfEngineBuiltins;
        for (uint32_t i = 0; i < modules.size(); ++i) {
            if (!modules[i].isBuiltinFunction || !modules[i].number || !BuiltinExecutables::isStamp(modules[i].keyHash))
                continue;
            unsigned which = modules[i].keyHash & 0xffff;
            while (modulesOfEngineBuiltins.size() <= which)
                modulesOfEngineBuiltins.append(0);
            modulesOfEngineBuiltins[which] = i + 1;
        }
        header.offsetOfModulesOfEngineBuiltins = builder.append(modulesOfEngineBuiltins);
        header.numberOfEngineBuiltins = modulesOfEngineBuiltins.size();
    }
    header.offsetOfInfos = builder.append(builder.m_infos);
    header.numberOfFunctions = builder.m_infos.size();
    header.offsetOfFunctionMetadataOffsets = builder.append(builder.m_functionMetadataOffsets);
    header.offsetOfRowsOfExecutables = builder.append(builder.m_rows);
    header.numberOfExecutables = builder.m_rows.size();
    header.numberOfIdentifiers = builder.m_identifiers.size();
    header.numberOfConstants = builder.m_constants.size();
    header.numberOfUnlinkedFunctions = builder.m_unlinkedFunctions.size();
    header.numberOfTopLevelCodes = builder.m_topLevelCodes.size();
    {
        Vector<uint32_t> starts;
        Vector<uint8_t> text;
        for (auto& name : builder.m_namesOfSources) {
            starts.append(text.size());
            text.append(name.span());
        }
        starts.append(text.size());
        header.offsetOfNamesOfSources = builder.append(starts);
        builder.m_out.appendVector(text);
        header.numberOfSources = builder.m_namesOfSources.size();
    }
    if (image->keysAreOmitted()) {
        // An executable that has a number records the index of its function. The others are created from their source at run time
        // and find their functions by key, so only those keys are kept.
        auto isLookedUp = [&](const ImageKey& key) {
            return key.record && key.kind != std::numeric_limits<uint32_t>::max() && !builder.m_infos[image->indexOfFunctionWith(key)].hasExecutable();
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
        header.offsetOfKeysOfImage = builder.append(kept);
        header.capacityOfKeysOfImage = capacity;
    }
    header.size = safeCast<uint32_t>(builder.m_out.size());
    memcpy(builder.m_out.mutableSpan().data(), &header, sizeof(header));
    if (Options::verboseAOTCompilation()) [[unlikely]]
        dataLogLn("AOT: program data: ", header.size, " bytes: strings ", header.sizeOfStrings, ", objects ", header.sizeOfObjects, "; ", header.numberOfFunctions, " functions, ", header.numberOfExecutables, " executables, ", header.numberOfUnlinkedFunctions, " unlinked functions, ", header.numberOfIdentifiers, " identifiers, ", header.numberOfConstants, " constants, ", header.numberOfSources, " sources");
    return WTF::move(builder.m_out);
}

// ---- When it runs.

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
    if (header.magic != expectedMagic || header.size > bytes.size() || header.offsetOfStrings > header.size || header.sizeOfStrings > header.size - header.offsetOfStrings)
        return std::nullopt;
    return std::pair { static_cast<size_t>(header.offsetOfStrings), static_cast<size_t>(header.sizeOfStrings) };
}

const ModuleOfProgram* ProgramData::moduleWithEntryOffset(uint32_t entryOffset) const
{
    auto all = modules();
    size_t index = std::ranges::lower_bound(all, entryOffset, { }, &ModuleOfProgram::entryOffset) - all.begin();
    return index != all.size() && all[index].entryOffset == entryOffset ? &all[index] : nullptr;
}

uint32_t ProgramData::numberOfExecutableOfFunction(uint32_t indexOfFunction) const
{
    const FunctionInfo& info = infos()[indexOfFunction];
    RELEASE_ASSERT(indexOfFunction < numberOfFunctions && info.oneMoreThanNumber() && !info.isTopLevelCode());
    return info.oneMoreThanNumber() - 1;
}

const RowOfExecutable& ProgramData::rowOfExecutableOfFunction(uint32_t indexOfFunction) const
{
    return rowOfExecutable(numberOfExecutableOfFunction(indexOfFunction));
}

String ProgramData::nameOfSource(uint32_t source) const
{
    RELEASE_ASSERT(source && source <= numberOfSources);
    auto* starts = at<uint32_t>(offsetOfNamesOfSources);
    auto* text = reinterpret_cast<const char8_t*>(starts + numberOfSources + 1);
    return String::fromUTF8(std::span { text + starts[source - 1], static_cast<size_t>(starts[source] - starts[source - 1]) });
}

// Reads the first two values that Builder::makePositions() wrote.
LineColumn ProgramData::whereFunctionStarts(uint32_t indexOfFunction) const
{
    RELEASE_ASSERT(indexOfFunction < numberOfFunctions);
    uint32_t word = functionMetadataOffsets()[indexOfFunction];
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

Vector<uint32_t> ProgramData::entryOffsetsOfModules() const
{
    Vector<uint32_t> result;
    for (auto& module : modules()) {
        if (!module.isBuiltinFunction && module.number)
            result.append(module.entryOffset);
    }
    return result;
}

std::span<const ImageKey> ProgramData::keysOfImage() const
{
    return spanAt<ImageKey>(offsetOfKeysOfImage, capacityOfKeysOfImage);
}

struct ProgramOfVM::Rest {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(Rest);

    std::unique_ptr<DecoderStringTable> ownStrings;
    std::unique_ptr<ObjectsOfProgramDecoder> decoder;
    // For the collector. (It must not look through the tables: that would touch all of their pages.)
    Vector<JSCell*> madeSinceLastCollection;
    BitVector executablesMade;
    Vector<uint32_t> identifiersMade;
    Vector<UnlinkedFunctionExecutable*> unlinkedFunctions;
    Vector<RefPtr<SourceProvider>> providers;
    UncheckedKeyHashMap<uint32_t, std::unique_ptr<SourceCode>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> sourcesOfShortExecutables;
    UncheckedKeyHashMap<uint32_t, FixedVector<UnlinkedStringJumpTable>> stringSwitchJumpTables;
    UncheckedKeyHashMap<uint32_t, FixedVector<IdentifierSet>> identifierSets;
};

template<typename T> static T* zeroedTable(size_t count)
{
    // (Pages are committed when they are first touched.)
    return static_cast<T*>(OSAllocator::reserveAndCommit(roundUpToMultipleOf(WTF::pageSize(), std::max<size_t>(count, 1) * sizeof(T)), OSAllocator::FastMallocPages));
}

template<typename T> static void freeTable(T* table, size_t count)
{
    OSAllocator::decommitAndRelease(table, roundUpToMultipleOf(WTF::pageSize(), std::max<size_t>(count, 1) * sizeof(T)));
}

ProgramOfVM* ProgramOfVM::of(VM& vm)
{
    if (!vm.m_aotProgram && ProgramData::get()) [[unlikely]]
        vm.m_aotProgram = makeUnique<ProgramOfVM>(vm);
    return vm.m_aotProgram.get();
}

ProgramOfVM::ProgramOfVM(VM& vm)
    : m_vm(vm)
    , m_data(*ProgramData::get())
    , m_identifiers(zeroedTable<UniquedStringImpl*>(m_data.numberOfIdentifiers))
    , m_executables(zeroedTable<FunctionExecutable*>(m_data.numberOfExecutables))
    , m_rest(makeUnique<Rest>())
{
    m_rest->unlinkedFunctions.fill(nullptr, m_data.numberOfUnlinkedFunctions);
    m_rest->providers.grow(m_data.numberOfModules);
    constexpr uint32_t initialCapacity = 1024;
    m_valuesOfConstants = static_cast<EncodedJSValue*>(fastZeroedMalloc(initialCapacity * sizeof(EncodedJSValue)));
    m_keysOfConstants = static_cast<uint32_t*>(fastZeroedMalloc(initialCapacity * sizeof(uint32_t)));
    m_maskOfConstants = initialCapacity - 1;
    m_rest->executablesMade.ensureSize(m_data.numberOfExecutables);
}

ProgramOfVM::~ProgramOfVM()
{
    for (uint32_t number : m_rest->identifiersMade)
        m_identifiers[number]->deref();
    fastFree(m_valuesOfConstants);
    fastFree(m_keysOfConstants);
    freeTable(m_identifiers, m_data.numberOfIdentifiers);
    freeTable(m_executables, m_data.numberOfExecutables);
}

DecoderStringTable& ProgramOfVM::strings()
{
    // (What is made of a string is remembered by the number of the identifier or the constant, so not by the string as well.)
    if (!m_rest->ownStrings)
        m_rest->ownStrings = makeUnique<DecoderStringTable>(m_data.strings(), DecoderStringTable::Slots::No);
    return *m_rest->ownStrings;
}

static ObjectsOfProgramDecoder& ensureDecoder(std::unique_ptr<ObjectsOfProgramDecoder>& decoder, VM& vm, const ProgramData& data, DecoderStringTable& strings)
{
    if (!decoder)
        decoder = makeUnique<ObjectsOfProgramDecoder>(vm, data.objects(), strings);
    return *decoder;
}

void ProgramOfVM::didMake(JSCell* cell)
{
    m_rest->madeSinceLastCollection.append(cell);
}

JSValue ProgramOfVM::constant(uint32_t number)
{
    RELEASE_ASSERT(number < m_data.numberOfConstants);
    for (uint32_t place = hashOfConstant(number);; ++place) {
        uint32_t key = m_keysOfConstants[place & m_maskOfConstants];
        if (key == number + 1)
            return JSValue::decode(m_valuesOfConstants[place & m_maskOfConstants]);
        if (!key)
            break;
    }
    DeferGC deferGC(m_vm);
    ObjectsOfProgramDecoder& decoder = ensureDecoder(m_rest->decoder, m_vm, m_data, strings());
    if (auto other = decoder.constantIsSameAs(number)) {
        JSValue value = constant(*other);
        addConstant(number, value);
        return value;
    }
    JSValue value = decoder.constant(number);
    RELEASE_ASSERT(value && (!value.isCell() || !value.isObject()));
    if (value.isCell())
        didMake(value.asCell());
    addConstant(number, value);
    return value;
}

void ProgramOfVM::addConstant(uint32_t number, JSValue value)
{
    auto add = [&](uint32_t key, EncodedJSValue encoded) {
        uint32_t place = hashOfConstant(key - 1);
        while (m_keysOfConstants[place & m_maskOfConstants])
            ++place;
        m_valuesOfConstants[place & m_maskOfConstants] = encoded;
        m_keysOfConstants[place & m_maskOfConstants] = key;
    };
    if (++m_numberOfConstantsMade * 2 > m_maskOfConstants) {
        uint32_t oldCapacity = m_maskOfConstants + 1;
        EncodedJSValue* oldValues = m_valuesOfConstants;
        uint32_t* oldKeys = m_keysOfConstants;
        m_valuesOfConstants = static_cast<EncodedJSValue*>(fastZeroedMalloc(oldCapacity * 2 * sizeof(EncodedJSValue)));
        m_keysOfConstants = static_cast<uint32_t*>(fastZeroedMalloc(oldCapacity * 2 * sizeof(uint32_t)));
        m_maskOfConstants = oldCapacity * 2 - 1;
        for (uint32_t i = 0; i < oldCapacity; ++i) {
            if (oldKeys[i])
                add(oldKeys[i], oldValues[i]);
        }
        fastFree(oldValues);
        fastFree(oldKeys);
    }
    add(number + 1, JSValue::encode(value));
}

UniquedStringImpl* ProgramOfVM::identifier(uint32_t number)
{
    RELEASE_ASSERT(number < m_data.numberOfIdentifiers);
    if (UniquedStringImpl* existing = m_identifiers[number])
        return existing;
    Identifier identifier = ensureDecoder(m_rest->decoder, m_vm, m_data, strings()).identifier(number);
    RELEASE_ASSERT(!identifier.isNull());
    identifier.impl()->ref();
    m_rest->identifiersMade.append(number);
    m_identifiers[number] = identifier.impl();
    return identifier.impl();
}

const Identifier& ProgramOfVM::identifierAsIdentifier(uint32_t number)
{
    static_assert(sizeof(Identifier) == sizeof(UniquedStringImpl*));
    if (number)
        identifier(number);
    return *reinterpret_cast<const Identifier*>(&m_identifiers[number]);
}

UnlinkedFunctionExecutable* ProgramOfVM::unlinkedFunction(uint32_t number, bool isShared)
{
    RELEASE_ASSERT(number < m_data.numberOfUnlinkedFunctions);
    if (UnlinkedFunctionExecutable* existing = m_rest->unlinkedFunctions[number])
        return existing;
    DeferGC deferGC(m_vm);
    UnlinkedFunctionExecutable* result = ensureDecoder(m_rest->decoder, m_vm, m_data, strings()).unlinkedFunction(number);
    RELEASE_ASSERT(result);
    if (isShared)
        result->becomeSharedByShortExecutables();
    didMake(result);
    m_rest->unlinkedFunctions[number] = result;
    return result;
}

UnlinkedCodeBlock* ProgramOfVM::topLevelCode(uint32_t number)
{
    // (It runs once in each instance, and whoever runs it lets go of it then. So it is not kept here.)
    RELEASE_ASSERT(number < m_data.numberOfTopLevelCodes);
    DeferGC deferGC(m_vm);
    UnlinkedCodeBlock* result = ensureDecoder(m_rest->decoder, m_vm, m_data, strings()).topLevelCode(number);
    RELEASE_ASSERT(result);
    return result;
}

UnlinkedCodeBlock* ProgramOfVM::topLevelCodeFor(const SourceCodeKey& key)
{
    SourceProvider& provider = key.source().provider();
    uint32_t id = provider.aotModuleID();
    if (!id || !provider.hasNoText())
        return nullptr;
    const ModuleOfProgram* module = m_data.moduleWithEntryOffset(id - 1);
    if (!module || module->isBuiltinFunction || !module->number || key.flagsBits() != module->keyFlags || !key.name().isEmpty() || key.functionConstructorParametersEndPosition() != -1)
        return nullptr;
    didLoadModule(provider);
    return topLevelCode(module->number - 1);
}

void ProgramOfVM::didLoadModule(SourceProvider& provider)
{
    const ModuleOfProgram* module = m_data.moduleWithEntryOffset(provider.aotModuleID() - 1);
    RELEASE_ASSERT(module);
    RefPtr<SourceProvider>& first = m_rest->providers[module - m_data.modules().data()];
    if (!first)
        first = &provider;
}

SourceProvider* ProgramOfVM::providerOfModule(uint32_t indexOfModule)
{
    RefPtr<SourceProvider>& provider = m_rest->providers[indexOfModule];
    if (!provider) {
        // Compiled code calls some builtins without a function object, so a frame can be of one that nothing has asked for.
        const ModuleOfProgram& module = m_data.modules()[indexOfModule];
        RELEASE_ASSERT(module.isBuiltinFunction);
        String text;
        if (BuiltinExecutables::isStamp(module.keyHash))
            text = StringImpl::createWithoutCopying(BuiltinExecutables::textOf(module.keyHash & 0xffff));
        provider = StringSourceProvider::create(text, SourceOrigin(), String(), SourceTaintedOrigin::Untainted);
        if (text.isNull())
            provider->setHasNoText();
        provider->setAOTModuleID(module.entryOffset + 1);
    }
    return provider.get();
}

const SourceCode& ProgramOfVM::sourceOfShortExecutable(uint32_t number)
{
    return *m_rest->sourcesOfShortExecutables.ensure(number, [&] {
        return makeUniqueWithoutFastMallocCheck<SourceCode>(RefPtr { providerOfModule(m_data.rowOfExecutable(number).module) }, 0, 0);
    }).iterator->value;
}

FunctionExecutable* ProgramOfVM::executable(uint32_t number)
{
    RELEASE_ASSERT(number < m_data.numberOfExecutables);
    if (FunctionExecutable* existing = m_executables[number])
        return existing;
    const RowOfExecutable& row = m_data.rowOfExecutable(number);
    DeferGC deferGC(m_vm);
    FunctionExecutable* result;
    if (row.isShort)
        result = FunctionExecutable::createInShortForm(m_vm, row.entry, row.index);
    else {
        SourceCode source { RefPtr { providerOfModule(row.module) }, static_cast<int>(row.startOffset), static_cast<int>(row.startOffset + row.sourceLength) };
        result = FunctionExecutable::create(m_vm, nullptr, source, unlinkedFunction(row.unlinkedFunction, false), NoIntrinsic, row.isInsideOrdinaryFunction);
        result->becomeSharedAcrossRealms(m_vm);
        for (auto kind : { CodeSpecializationKind::CodeForCall, CodeSpecializationKind::CodeForConstruct }) {
            if (uint64_t entry = row.entry[static_cast<unsigned>(kind)])
                result->setAOTCode(kind, entry, row.index[static_cast<unsigned>(kind)]);
        }
    }
    didMake(result);
    m_rest->executablesMade.quickSet(number);
    m_executables[number] = result;
    return result;
}

FunctionExecutable* ProgramOfVM::builtinFunctionFor(uint32_t entryOffset, unsigned embedderStamp, const String& text, const SourceOrigin& sourceOrigin, const String& sourceURL)
{
    if (BytecodeOrderRecorder::ofVM(m_vm))
        return nullptr;
    const ModuleOfProgram* module = m_data.moduleWithEntryOffset(entryOffset);
    if (!module || !module->isBuiltinFunction || !module->number || module->keyHash != embedderStamp || module->keyLength != text.length())
        return nullptr;
    RefPtr<SourceProvider>& provider = m_rest->providers[module - m_data.modules().data()];
    if (!provider) {
        provider = StringSourceProvider::create(text, sourceOrigin, String { sourceURL }, SourceTaintedOrigin::Untainted);
        provider->setAOTModuleID(entryOffset + 1);
    }
    return executable(module->number - 1);
}

FunctionExecutable* ProgramOfVM::engineBuiltinFor(unsigned index, std::span<const Latin1Character> text)
{
    uint32_t oneMoreThanModule = index < m_data.numberOfEngineBuiltins ? m_data.at<uint32_t>(m_data.offsetOfModulesOfEngineBuiltins)[index] : 0;
    if (!oneMoreThanModule)
        return nullptr;
    const ModuleOfProgram& module = m_data.modules()[oneMoreThanModule - 1];
    if (BytecodeOrderRecorder::ofVM(m_vm) || module.keyLength != text.size())
        return nullptr;
    RefPtr<SourceProvider>& provider = m_rest->providers[oneMoreThanModule - 1];
    if (!provider) {
        provider = StringSourceProvider::create(StringImpl::createWithoutCopying(text), SourceOrigin(), String(), SourceTaintedOrigin::Untainted);
        provider->setAOTModuleID(module.entryOffset + 1);
    }
    return executable(module.number - 1);
}

const UnlinkedStringJumpTable& ProgramOfVM::stringSwitchJumpTable(uint32_t offsetOfTables, unsigned which)
{
    return m_rest->stringSwitchJumpTables.ensure(offsetOfTables, [&] {
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

const IdentifierSet& ProgramOfVM::identifierSet(uint32_t offsetOfSets, unsigned which)
{
    return m_rest->identifierSets.ensure(offsetOfSets, [&] {
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
void ProgramOfVM::visit(Visitor& visitor, CollectionScope scope)
{
    if (scope == CollectionScope::Eden) {
        for (JSCell* cell : m_rest->madeSinceLastCollection)
            visitor.appendUnbarriered(cell);
        return;
    }
    for (uint32_t i = 0; i <= m_maskOfConstants; ++i) {
        if (JSValue value = JSValue::decode(m_valuesOfConstants[i]); m_keysOfConstants[i] && value.isCell())
            visitor.appendUnbarriered(value.asCell());
    }
    m_rest->executablesMade.forEachSetBit([&](size_t number) {
        visitor.appendUnbarriered(m_executables[number]);
    });
    for (auto* function : m_rest->unlinkedFunctions) {
        if (function)
            visitor.appendUnbarriered(function);
    }
}
template void ProgramOfVM::visit(AbstractSlotVisitor&, CollectionScope);
template void ProgramOfVM::visit(SlotVisitor&, CollectionScope);

void ProgramOfVM::didFinishCollection()
{
    m_rest->madeSinceLastCollection.clear();
    if (Options::verboseAOTCompilation()) [[unlikely]] {
        UncheckedKeyHashMap<const ClassInfo*, std::pair<size_t, size_t>> byClass;
        auto count = [&](JSCell* cell) {
            auto& entry = byClass.add(cell->classInfo(), std::pair<size_t, size_t> { }).iterator->value;
            entry.first++;
            entry.second += cell->cellSize();
        };
        for (uint32_t i = 0; i <= m_maskOfConstants; ++i) {
            if (JSValue value = JSValue::decode(m_valuesOfConstants[i]); m_keysOfConstants[i] && value.isCell())
                count(value.asCell());
        }
        m_rest->executablesMade.forEachSetBit([&](size_t number) { count(m_executables[number]); });
        for (auto* function : m_rest->unlinkedFunctions) {
            if (function)
                count(function);
        }
        dataLog("AOT: made so far: ", m_rest->identifiersMade.size(), " of ", m_data.numberOfIdentifiers, " identifiers, ", m_numberOfConstantsMade, " of ", m_data.numberOfConstants, " constants, ", m_rest->executablesMade.bitCount(), " of ", m_data.numberOfExecutables, " executables:");
        for (auto& [info, entry] : byClass)
            dataLog(" ", info->className, " ", entry.first, " (", entry.second, " bytes)");
        dataLogLn("; ", m_rest->stringSwitchJumpTables.size(), " + ", m_rest->identifierSets.size(), " tables of functions");
    }
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
