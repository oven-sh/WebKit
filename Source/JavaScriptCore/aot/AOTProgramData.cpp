/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTProgramData.h"

#if ENABLE(AOT)

#include "AOTImage.h"
#include "AOTOpcodeTraits.h"
#include "AOTProgram.h"
#include "AOTRuntime.h"
#include "AOTTypedThreads.h"
#include "BlockDirectoryInlines.h"
#include "BuiltinExecutables.h"
#include "BytecodeStructs.h"
#include "CachedTypes.h"
#include "FunctionExecutable.h"
#include "JSBigInt.h"
#include "JSCInlines.h"
#include "JSTemplateObjectDescriptor.h"
#include "MarkedBlockInlines.h"
#include "SourceCodeKey.h"
#include "SubspaceInlines.h"
#include "UnlinkedFunctionCodeBlock.h"
#include "UnlinkedFunctionExecutable.h"
#include "UnlinkedModuleProgramCodeBlock.h"
#include <wtf/BitVector.h>
#include <wtf/MonotonicTime.h>
#include <wtf/OSAllocator.h>
#include <wtf/SegmentedVector.h>
#include <wtf/TZoneMallocInlines.h>

namespace JSC { namespace AOT {

WTF_MAKE_TZONE_ALLOCATED_IMPL(VMProgram);

static constexpr uint32_t sourceNameRestartInterval = 16;

struct SourceNameHeader {
    uint16_t firstBytesDistance;
    uint16_t sharedAndIsRelative;
    uint16_t restAndIsASCII;
};

namespace {

void appendVarint(Vector<uint8_t>& bytes, uint64_t value)
{
    while (value >= 0x80) {
        bytes.append(static_cast<uint8_t>(value) | 0x80);
        value >>= 7;
    }
    bytes.append(static_cast<uint8_t>(value));
}

class PositionEncoder {
public:
    explicit PositionEncoder(Vector<uint8_t>& stream)
        : m_stream(stream)
    {
    }

    void append(LineColumn position, uint32_t source)
    {
        auto zigZagEncode = [](int64_t value) {
            return static_cast<uint64_t>(value << 1) ^ static_cast<uint64_t>(value >> 63);
        };
        if (position.line == m_line && source == m_source)
            appendVarint(m_stream, zigZagEncode(static_cast<int64_t>(position.column) - m_column) << 1 | 1);
        else {
            appendVarint(m_stream, zigZagEncode(static_cast<int64_t>(position.line) - m_line) << 2 | (source != m_source) << 1);
            if (source != m_source)
                appendVarint(m_stream, source);
            appendVarint(m_stream, position.column);
        }
        m_line = position.line;
        m_column = position.column;
        m_source = source;
    }

    struct State {
        int64_t line { 0 };
        int64_t column { 0 };
        uint32_t source { 0 };
    };
    State state() const { return { m_line, m_column, m_source }; }
    void setState(const State& state)
    {
        m_line = state.line;
        m_column = state.column;
        m_source = state.source;
    }

private:
    Vector<uint8_t>& m_stream;
    int64_t m_line { 0 };
    int64_t m_column { 0 };
    uint32_t m_source { 0 };
};

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
        m_indexAndFlags.fill(0, image.numberOfFunctions());
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

    static uint64_t hashForAppendOnce(std::span<const uint8_t> bytes)
    {
        uint64_t hash = 1469598103934665603ull;
        for (uint8_t byte : bytes)
            hash = (hash ^ byte) * 1099511628211ull;
        return hash;
    }

    uint32_t appendOnce(std::span<const uint8_t> bytes, size_t alignment)
    {
        return appendOnce(bytes, alignment, hashForAppendOnce(bytes));
    }

    uint32_t appendOnce(std::span<const uint8_t> bytes, size_t alignment, uint64_t hash)
    {
        auto& withHash = m_inlinedCopies.add(hash | 1, Vector<std::pair<uint32_t, uint32_t>, 1> { }).iterator->value;
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

    uint32_t numberOfSource(const UTF8CString& name)
    {
        return m_sources.ensure(name, [&] {
            m_sourceNames.append(name);
            return static_cast<uint32_t>(m_sourceNames.size());
        }).iterator->value;
    }

    struct SourceWithHash {
        const UTF8CString& name;
        unsigned hash;
    };

    struct SourceWithHashTranslator {
        static unsigned hash(const SourceWithHash& source) { return source.hash; }
        static bool equal(const UTF8CString& name, const SourceWithHash& source) { return name == source.name; }
        static void translate(UTF8CString& location, const SourceWithHash& source, unsigned) { location = source.name; }
    };

    static Vector<uint8_t> typedPositionsOf(const FunctionReportableSites& sites, std::span<const uint32_t> sourceNumbers)
    {
        RELEASE_ASSERT(!sites.typedPositions.isEmpty());
        Vector<uint8_t> stream;
        appendVarint(stream, sites.typedPositions[0].line);
        appendVarint(stream, sites.typedPositions[0].column);
        appendVarint(stream, sites.typedPositions.size());
        PositionEncoder::State start { sites.typedPositions[0].line, sites.typedPositions[0].column, sourceNumbers[sites.typedPositions[0].source] };
        appendVarint(stream, start.source);
        Vector<uint8_t> positions;
        size_t lastRunEnd = 0;
        PositionEncoder encoder(positions);
        bool hasSitesWithCode = sites.typedPositions.size() > 1 + sites.numberOfSitesWithoutCode;
        Vector<PositionEncoder::State, 8> states { start };
        for (unsigned site = 1; site < sites.typedPositions.size(); ++site) {
            if (!(site % FunctionMetadata::sitesBetweenPositionCheckpoints)) {
                size_t bytesSinceLast = positions.size() - lastRunEnd;
                RELEASE_ASSERT(bytesSinceLast <= std::numeric_limits<uint16_t>::max());
                stream.append(static_cast<uint8_t>(bytesSinceLast));
                stream.append(static_cast<uint8_t>(bytesSinceLast >> 8));
                lastRunEnd = positions.size();
                states.fill(start);
            }
            auto& position = sites.typedPositions[site];
            uint32_t frame = hasSitesWithCode ? position.frame : 0;
            while (frame >= states.size())
                states.append(start);
            encoder.setState(states[frame]);
            encoder.append({ position.line, position.column }, sourceNumbers[position.source]);
            states[frame] = encoder.state();
        }
        stream.appendVector(positions);
        return stream;
    }

    Vector<unsigned, 2> sourceHashesFor(uint32_t function) const
    {
        return WTF::map<2>(m_reportableSites[function].typedSourceNames, [&](const UTF8CString& name) {
            return name.hash();
        });
    }

    uint32_t numberOfSource(const UTF8CString& name, unsigned hash)
    {
        return m_sources.ensure<SourceWithHashTranslator>(SourceWithHash { name, hash }, [&] {
            m_sourceNames.append(name);
            return static_cast<uint32_t>(m_sourceNames.size());
        }).iterator->value;
    }

    const FunctionReportableSites& reportableSitesOf(uint32_t function) const { return m_reportableSites[function]; }

    uint32_t makePositions(uint32_t index, UnlinkedCodeBlock* codeBlock, unsigned sourceOffset, LineStartTable& lineStarts)
    {
        Vector<uint8_t> stream;
        auto& sites = m_positions.sites[index];
        Vector<uint32_t> offsets = sites.offsets;
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
        PositionEncoder encoder(stream);
        auto appendPosition = [&](LineColumn inModule) {
            UTF8CString name;
            LineColumn position = inModule;
            uint32_t source = 0;
            if (m_positions.find(m_moduleEntryOffset, inModule, name, position))
                source = numberOfSource(name);
            else
                position = inModule;
            encoder.append(position, source);
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

    Vector<UnlinkedHandlerInfo> typedExceptionHandlers(uint32_t index) const
    {
        Vector<UnlinkedHandlerInfo> handlers;
        for (auto& [siteAndFlag, handler] : m_reportableSites[index].typedCatchEntrypointsForCallSites) {
            uint32_t site = siteAndFlag & ~FunctionReportableSites::handlerFlags;
            if (!handlers.isEmpty() && handlers.last().end == site && handlers.last().target == handler) {
                handlers.last().end = site + 1;
                continue;
            }
            handlers.grow(handlers.size() + 1);
            zeroSpan(handlers.mutableSpan().last(1));
            handlers.last().start = site;
            handlers.last().end = site + 1;
            handlers.last().target = handler;
            handlers.last().setType(siteAndFlag & FunctionReportableSites::handlerTakesException ? HandlerType::Finally : siteAndFlag & FunctionReportableSites::handlerIgnoresValue ? HandlerType::SynthesizedCatch : HandlerType::Catch);
        }
        return handlers;
    }

    void fillMetadata(uint32_t index, UnlinkedCodeBlock* codeBlock, LineStartTable& lineStarts)
    {
        if (m_functionMetadataOffsets[index])
            return;
        using Metadata = FunctionMetadata;
        RELEASE_ASSERT(codeBlock->instructions().size() < (1u << (32 - Metadata::instructionsSizeShift)));
        Vector<uint32_t, 16> words { static_cast<uint32_t>(codeBlock->instructions().size()) << Metadata::instructionsSizeShift | (codeBlock->isBuiltinFunction() ? Metadata::isBuiltinFunction : 0) };
        RELEASE_ASSERT(codeBlock->codeType() != FunctionCode);
        words[0] |= Metadata::ExpressionInfo;
        words.append(makePositions(index, codeBlock, 0, lineStarts));
        if (auto handlers = typedExceptionHandlers(index); !handlers.isEmpty()) {
            words[0] |= Metadata::Handlers;
            words.append(append(handlers));
            words.append(handlers.size());
        } else if (size_t count = codeBlock->numberOfExceptionHandlers()) {
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

    void fillInfo(const ImageView::Function& function, UnlinkedCodeBlock* codeBlock, LineStartTable& lineStarts)
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
        RELEASE_ASSERT(info.sites);
        info.stateNumber = m_reportableSites[function.index].stateNumber;
        RELEASE_ASSERT(info.stateNumber);
        info.hasInlineFrames = function.hasInlineFrames;
        fillMetadata(function.index, codeBlock, lineStarts);
    }

    void recordTypedIdentifiers()
    {
        BitVector isRecorded(m_identifiers.size());
        for (auto& sites : m_reportableSites) {
            RELEASE_ASSERT(sites.typedIdentifierIndices.size() == sites.typedIdentifierNames.size());
            for (unsigned i = 0; i < sites.typedIdentifierIndices.size(); ++i) {
                if (isRecorded.set(sites.typedIdentifierIndices[i]) || sites.typedIdentifierNames[i].isNull())
                    continue;
                AtomString name(sites.typedIdentifierNames[i]);
                m_typedIdentifiers.append(name);
                UniquedStringImpl* inTable = m_identifiers[sites.typedIdentifierIndices[i]];
                RELEASE_ASSERT(!inTable || inTable == name.impl());
                if (!inTable)
                    m_identifierIndices.add(name.impl(), sites.typedIdentifierIndices[i]);
            }
        }
    }

    struct PrecomputedTypedInfo {
        uint64_t hash { 0 };
        uint64_t closureWord { 0 };
        Vector<uint8_t> handlersAndPositions;
    };

    void precomputeTypedInfo(const ImageView::Function& function, const Vector<uint8_t>& positions, PrecomputedTypedInfo& ahead)
    {
        auto& sites = m_reportableSites[function.index];
        FunctionInfo& info = m_infos[function.index];
        info.sites = safeCast<uint32_t>(std::bit_cast<uintptr_t>(function.sites));
        RELEASE_ASSERT(info.sites && function.numSlots <= FunctionInfo::maxEncodedSlots);
        info.hasInlineFrames = function.hasInlineFrames;
        m_indexAndFlags[function.index] = FunctionInfo::encodeTypedFunction(sites.typedFunction);
        info.isTyped = true;
        info.stateNumber = sites.stateNumber;
        RELEASE_ASSERT(info.stateNumber || !function.numSlots);
        auto handlers = typedExceptionHandlers(function.index);
        if (handlers.isEmpty()) {
            info.hasNoHandlers = true;
            ahead.hash = hashForAppendOnce(positions.span());
            return;
        }
        uint32_t greatest = 0;
        uint32_t endOfLast = 0;
        for (auto& handler : handlers) {
            RELEASE_ASSERT(handler.start >= endOfLast && handler.end > handler.start && handler.type() != HandlerType::SynthesizedFinally);
            greatest = std::max(greatest, std::max(handler.end, handler.target));
            endOfLast = handler.end;
        }
        unsigned width = greatest <= UINT8_MAX ? 1 : greatest <= UINT16_MAX ? 2 : 4;
        bool someHandlerIgnoresValue = handlers.containsIf([](auto& handler) {
            return handler.type() == HandlerType::SynthesizedCatch;
        });
        Vector<uint8_t> handlerBytes { static_cast<uint8_t>(width | (someHandlerIgnoresValue ? FunctionMetadata::someHandlerIgnoresValue : 0)) };
        for (auto& handler : handlers) {
            bool takesException = handler.type() == HandlerType::Finally;
            for (uint32_t number : { takesException ? handler.end : handler.start, takesException ? handler.start : handler.end, handler.target }) {
                for (unsigned i = 0; i < width; ++i)
                    handlerBytes.append(static_cast<uint8_t>(number >> 8 * i));
            }
        }
        if (someHandlerIgnoresValue) {
            size_t startOfBits = handlerBytes.size();
            handlerBytes.grow(startOfBits + (handlers.size() + 7) / 8);
            zeroSpan(handlerBytes.mutableSpan().subspan(startOfBits));
            for (size_t i = 0; i < handlers.size(); ++i) {
                if (handlers[i].type() == HandlerType::SynthesizedCatch)
                    handlerBytes[startOfBits + i / 8] |= 1 << i % 8;
            }
        }
        appendVarint(ahead.handlersAndPositions, handlerBytes.size());
        ahead.handlersAndPositions.appendVector(handlerBytes);
        ahead.handlersAndPositions.appendVector(positions);
        ahead.hash = hashForAppendOnce(ahead.handlersAndPositions.span());
    }

    void fillTypedInfos(std::span<const std::optional<ImageView::Function>> functions, std::span<const Vector<uint8_t>> positions, std::span<const PrecomputedTypedInfo> precomputed)
    {
        struct Row {
            uint64_t hash { 0 };
            uint32_t number { 0 };
        };
        auto bytesOf = [&](uint32_t number) {
            return precomputed[number].handlersAndPositions.isEmpty() ? positions[number].span() : precomputed[number].handlersAndPositions.span();
        };
        Vector<Row> rows;
        rows.reserveInitialCapacity(functions.size());
        for (uint32_t number = 0; number < functions.size(); ++number) {
            if (functions[number])
                rows.append(Row { precomputed[number].hash, number });
        }
        Typed::sortOnPool(rows.mutableSpan(), [](const Row& a, const Row& b) {
            return a.hash != b.hash ? a.hash < b.hash : a.number < b.number;
        });
        Vector<uint32_t> firstWithBytesOf;
        Vector<uint32_t> newByteSizes;
        Vector<uint32_t> offsets;
        firstWithBytesOf.fill(0, functions.size());
        newByteSizes.fill(0, functions.size());
        offsets.fill(0, functions.size());
        constexpr size_t rowsInChunk = 4096;
        Typed::forEachIndexInParallel((rows.size() + rowsInChunk - 1) / rowsInChunk, [&](unsigned chunk) {
            size_t end = std::min(rows.size(), (static_cast<size_t>(chunk) + 1) * rowsInChunk);
            size_t at = static_cast<size_t>(chunk) * rowsInChunk;
            while (at && at < end && rows[at].hash == rows[at - 1].hash)
                ++at;
            Vector<uint32_t, 4> firsts;
            while (at < end) {
                uint64_t hash = rows[at].hash;
                auto writtenBefore = m_inlinedCopies.find(hash | 1);
                firsts.shrink(0);
                for (; at < rows.size() && rows[at].hash == hash; ++at) {
                    uint32_t number = rows[at].number;
                    auto bytes = bytesOf(number);
                    RELEASE_ASSERT(!bytes.empty());
                    firstWithBytesOf[number] = number;
                    bool isWritten = false;
                    if (writtenBefore != m_inlinedCopies.end()) {
                        for (auto [offset, size] : writtenBefore->value) {
                            if (size == bytes.size() && equalSpans(m_out.span().subspan(offset, size), bytes)) {
                                offsets[number] = offset;
                                isWritten = true;
                                break;
                            }
                        }
                    }
                    if (isWritten)
                        continue;
                    size_t same = firsts.findIf([&](uint32_t first) {
                        return equalSpans(bytesOf(first), bytes);
                    });
                    if (same != notFound) {
                        firstWithBytesOf[number] = firsts[same];
                        continue;
                    }
                    firsts.append(number);
                    newByteSizes[number] = safeCast<uint32_t>(bytes.size());
                }
            }
        });
        size_t sizeOfOutput = m_out.size();
        for (uint32_t number = 0; number < functions.size(); ++number) {
            if (!newByteSizes[number])
                continue;
            offsets[number] = safeCast<uint32_t>(sizeOfOutput);
            sizeOfOutput += newByteSizes[number];
        }
        m_out.grow(sizeOfOutput);
        Typed::forEachIndexInParallel(functions.size(), [&](unsigned number) {
            if (!functions[number])
                return;
            uint32_t offset = offsets[firstWithBytesOf[number]];
            if (newByteSizes[number])
                memcpySpan(m_out.mutableSpan().subspan(offset, newByteSizes[number]), bytesOf(number));
            if (precomputed[number].handlersAndPositions.isEmpty()) {
                RELEASE_ASSERT(!(offset >> 31));
                m_functionMetadataOffsets[functions[number]->index] = offset << 1 | FunctionMetadata::isTypedFunctionPositions;
                return;
            }
            RELEASE_ASSERT(!(offset >> 30));
            m_functionMetadataOffsets[functions[number]->index] = offset << 2 | FunctionMetadata::isTypedFunctionHandlersAndPositions;
        });
    }

    void fillTopLevelCodeInfo(const ImageView::Function& function, UnlinkedCodeBlock* codeBlock, uint32_t numberOfTopLevelCode, LineStartTable& lineStarts)
    {
        fillInfo(function, codeBlock, lineStarts);
        FunctionInfo& info = m_infos[function.index];
        m_indexAndFlags[function.index] = FunctionInfo::encodeIndexAndFlags(numberOfTopLevelCode + 1, CodeSpecializationKind::CodeForCall, codeBlock->codeType());
        info.isHiddenInStackTraces = m_reportableSites[function.index].typedFunction != FunctionReportableSites::notTyped;
    }

    void dropUnreferencedVariableNames(UnlinkedCodeBlock* codeBlock, DynamicallyResolvedNames& lookedUp, const Vector<uint32_t>* exportedByModule = nullptr)
    {
        DynamicallyResolvedNames own;
        for (const auto& instruction : codeBlock->instructions()) {
            switch (instruction->opcodeID()) {
            case op_call_direct_eval:
            case op_resolve_scope_for_hoisting_func_decl_in_eval:
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
                RELEASE_ASSERT(!(traitsOf(instruction->opcodeID()) & OpcodeTraits::LooksUpVariableByName));
                break;
            }
        }

        SymbolTable* moduleValue = nullptr;
        if (auto* moduleCode = dynamicDowncast<UnlinkedModuleProgramCodeBlock>(codeBlock))
            moduleValue = dynamicDowncast<SymbolTable>(moduleCode->constantRegister(VirtualRegister(moduleCode->moduleEnvironmentSymbolTableConstantRegisterOffset())).get());
        if (moduleValue && exportedByModule && !own.mayBeAnything) {
            UncheckedKeyHashSet<uint32_t, WTF::IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> exported;
            for (uint32_t offset : *exportedByModule)
                exported.add(offset);
            moduleValue->retainOnly([&](UniquedStringImpl* name, const SymbolTableEntry& entry) {
                return name->isSymbol() || own.names.contains(name) || (entry.varOffset().isScope() && exported.contains(entry.scopeOffset().offset()));
            });
        }
        for (auto& constant : codeBlock->constantRegisters()) {
            auto* table = constant.get().isCell() ? dynamicDowncast<SymbolTable>(constant.get().asCell()) : nullptr;
            if (!table || table == moduleValue)
                continue;
            RELEASE_ASSERT(m_processedSymbolTables.add(table).isNewEntry);
            if (!own.mayBeAnything)
                table->retainOnlyNames(own.names);
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

    VM& m_vm;
    const ImageView& m_image;
    const ProgramData::RetainedPositions& m_positions;
    std::span<const FunctionReportableSites> m_reportableSites;
    uint32_t m_moduleEntryOffset { 0 };

    Vector<uint8_t> m_out;
    UncheckedKeyHashMap<uint64_t, Vector<std::pair<uint32_t, uint32_t>, 1>> m_inlinedCopies;
    Vector<FunctionInfo> m_infos;
    Vector<uint32_t> m_indexAndFlags;
    Vector<uint32_t> m_functionMetadataOffsets;
    Vector<UniquedStringImpl*> m_identifiers;
    Vector<AtomString> m_typedIdentifiers;
    Vector<RefPtr<AtomStringImpl>> m_switchAtoms;
    UncheckedKeyHashMap<UniquedStringImpl*, uint32_t> m_identifierIndices;
    Vector<JSValue> m_constants;
    Vector<UnlinkedCodeBlock*> m_topLevelCodes;
    UncheckedKeyHashMap<UTF8CString, uint32_t> m_sources;
    Vector<UTF8CString> m_sourceNames;
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

void ProgramData::release(Vector<FunctionReportableSites>& sites)
{
    Typed::forEachIndexInParallel(sites.size(), [&](unsigned i) {
        auto& ofFunction = sites[i];
        ofFunction.offsets.clear();
        ofFunction.constructions.clear();
        ofFunction.identifierIndices.clear();
        ofFunction.typedIdentifierIndices.clear();
        ofFunction.typedIdentifierNames.removeAllMatching([](const String& name) {
            return !name.impl() || !name.impl()->isAtom();
        });
        ofFunction.typedCatchEntrypointsForCallSites.clear();
        ofFunction.typedPositions.clear();
        ofFunction.parameterCheckSites.clear();
        ofFunction.typedSourceNames.clear();
        ofFunction.constantIndices.clear();
    });
}

Vector<uint8_t> ProgramData::build(VM& vm, std::span<const uint8_t> strings, std::span<const uint8_t> payload, std::span<const uint32_t> moduleEntryOffsets, std::span<const uint8_t> codeImage, const RetainedPositions& positions, std::span<const FunctionReportableSites> reportableSites, std::span<const std::optional<Vector<uint32_t>>> variablesExportedByModules)
{
    MonotonicTime stepStart = MonotonicTime::now();
    Typed::PhaseTimes phaseTimes("program data"_s);
    auto endStep = [&](ASCIILiteral name) {
        if (!Typed::shouldPrintTimings()) [[likely]]
            return;
        MonotonicTime now = MonotonicTime::now();
        dataLogLn("AOT: - ", name, " took ", static_cast<unsigned>((now - stepStart).milliseconds()), " ms");
        stepStart = now;
        phaseTimes.didFinish(name);
    };
    auto image = ImageView::tryCreate(codeImage, nullptr);
    if (!image || payload.empty() || moduleEntryOffsets.empty())
        return { };
    DeferGC deferGC(vm);
    Builder builder(vm, *image, positions, reportableSites);

    endStep("the view of the image, and the builder"_s);
    DecoderStringTable table(strings);
    EncoderStringTable encoderStringTable;
    for (uint32_t ordinal = 0; ordinal < table.count(); ++ordinal)
        RELEASE_ASSERT(encoderStringTable.ordinalFor(table.atomFor(vm, ordinal).get()) == ordinal);

    endStep("the table of strings"_s);
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
        LineStartTable lineStarts;
        auto setLineStarts = [&](const LineStarts& codeLineStarts) {
            lineStarts.setLineStarts(codeLineStarts ? LineStarts { codeLineStarts } : LineStartTable::encode(Vector<unsigned> { 0 }));
        };
        UnlinkedCodeBlock* codeBlock = decodeModuleForProgramData(decoder.get(), key);
        if (codeBlock && (!key.name().isEmpty() || key.functionConstructorParametersEndPosition() != -1))
            codeBlock = nullptr;
        if (!codeBlock) {
            dataLogLnIf(Options::verboseAOTCompilation(), "AOT: the code of a module could not be decoded");
            ok = false;
            continue;
        }
        RELEASE_ASSERT(!codeBlock->numberOfFunctionDecls() && !codeBlock->numberOfFunctionExprs());
        setLineStarts(uncheckedDowncast<UnlinkedGlobalCodeBlock>(codeBlock)->lineStarts());
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

    endStep("the modules"_s);
    builder.mergeSymbolTables();
    endStep("mergeSymbolTables"_s);
    builder.recordTypedIdentifiers();

    endStep("recordTypedIdentifiers"_s);
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
        Vector<uint8_t> encoded = encodeProgramObjects(vm, encoderStringTable, objects);
        RELEASE_ASSERT(encoderStringTable.count() == table.count());
        header.objectsOffset = builder.append(encoded.span(), 16);
        header.objectsSize = safeCast<uint32_t>(encoded.size());
    }
    header.modulesOffset = builder.append(modules);
    header.numberOfModules = modules.size();
    endStep("strings, identifiers, constants and modules"_s);
    Vector<uint64_t> typedEntries;
    auto keys = image->keys();
    Vector<std::optional<ImageView::Function>> typedFunctions(keys.size());
    Vector<Vector<unsigned, 2>> sourceHashes(keys.size());
    Typed::forEachIndexInParallel(keys.size(), [&](unsigned i) {
        auto& key = keys[i];
        if (key.record && !(key.record & ImageKey::isEntryInsideFunction) && key.kind != std::numeric_limits<uint32_t>::max() && key.kind & ImageKey::typedFunction)
            typedFunctions[i] = image->find(key);
        if (typedFunctions[i])
            sourceHashes[i] = builder.sourceHashesFor(typedFunctions[i]->index);
    });
    endStep("looking up the typed functions"_s);
    Vector<Vector<uint32_t, 2>> sourceNumbers(keys.size());
    {
        constexpr size_t keysInChunk = 2048;
        unsigned numberOfChunks = (keys.size() + keysInChunk - 1) / keysInChunk;
        auto forEachTypedFunctionIn = [&](unsigned chunk, const auto& functor) {
            size_t end = std::min(keys.size(), (static_cast<size_t>(chunk) + 1) * keysInChunk);
            for (size_t i = static_cast<size_t>(chunk) * keysInChunk; i < end; ++i) {
                if (typedFunctions[i])
                    functor(i);
            }
        };
        Vector<Vector<std::pair<const UTF8CString*, unsigned>>> chunkNames(numberOfChunks);
        Typed::forEachIndexInParallel(numberOfChunks, [&](unsigned chunk) {
            auto& namesMet = chunkNames[chunk];
            UncheckedKeyHashMap<uint64_t, Vector<uint32_t, 1>, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> ordinalsWithHash;
            forEachTypedFunctionIn(chunk, [&](size_t i) {
                auto& names = builder.reportableSitesOf(typedFunctions[i]->index).typedSourceNames;
                RELEASE_ASSERT(sourceHashes[i].size() == names.size());
                sourceNumbers[i] = Vector<uint32_t, 2>(names.size(), [&](size_t index) {
                    auto& ordinals = ordinalsWithHash.add(sourceHashes[i][index], Vector<uint32_t, 1> { }).iterator->value;
                    for (uint32_t ordinal : ordinals) {
                        if (*namesMet[ordinal].first == names[index])
                            return ordinal;
                    }
                    ordinals.append(namesMet.size());
                    namesMet.append({ &names[index], sourceHashes[i][index] });
                    return ordinals.last();
                });
            });
        });
        Vector<Vector<uint32_t>> chunkNumbers(numberOfChunks);
        for (unsigned chunk = 0; chunk < numberOfChunks; ++chunk) {
            chunkNumbers[chunk] = WTF::map(chunkNames[chunk], [&](auto& name) {
                return builder.numberOfSource(*name.first, name.second);
            });
        }
        Typed::forEachIndexInParallel(numberOfChunks, [&](unsigned chunk) {
            forEachTypedFunctionIn(chunk, [&](size_t i) {
                for (uint32_t& number : sourceNumbers[i])
                    number = chunkNumbers[chunk][number];
            });
        });
    }
    endStep("the numbers of the sources"_s);
    Vector<Vector<uint8_t>> typedPositions(keys.size());
    Typed::forEachIndexInParallel(keys.size(), [&](unsigned i) {
        if (typedFunctions[i])
            typedPositions[i] = Builder::typedPositionsOf(builder.reportableSitesOf(typedFunctions[i]->index), sourceNumbers[i].span());
    });
    endStep("the typed positions"_s);
    Vector<Builder::PrecomputedTypedInfo> precomputed(keys.size());
    Typed::forEachIndexInParallel(keys.size(), [&](unsigned i) {
        auto& key = keys[i];
        if (key.record & ImageKey::isEntryInsideFunction) {
            precomputed[i].closureWord = typedFunctionClosureWord(codeImage, key.start, EntryWord::encode(static_cast<uint64_t>(key.record & ~ImageKey::isEntryInsideFunction), Convention { key.module >> 8 & 1 ? Signature::List : Signature::Registers, static_cast<uint8_t>(key.module), true })) | static_cast<uint64_t>(key.module >> 9 & 1) << ClosureWord::hasCompatibleResultsMaskBit;
            return;
        }
        if (auto& function = typedFunctions[i]) {
            builder.precomputeTypedInfo(*function, typedPositions[i], precomputed[i]);
            if (key.kind == ImageKey::typedFunction)
                precomputed[i].closureWord = typedFunctionClosureWord(codeImage, key.start, function->entry) | static_cast<uint64_t>(function->hasValueEntry) << ClosureWord::hasValueEntryBit | static_cast<uint64_t>(function->hasCompatibleResultsMask) << ClosureWord::hasCompatibleResultsMaskBit;
        }
    });
    endStep("the typed infos made ahead"_s);
    Vector<uint8_t> parameterChecks;
    uint32_t numberOfParameterChecks = 0;
    Vector<uint32_t> sourceNameOffsets;
    Vector<uint8_t> sourceNameBytes;
    Ref sourceNameThread = Thread::create("AOT image"_s, [&] {
        {
            using Definition = std::array<uint32_t, 4>;
            Vector<std::tuple<uint32_t, uint32_t, Definition>> rows;
            Vector<Definition> definitions;
            for (size_t i = 0; i < keys.size(); ++i) {
                if (!typedFunctions[i] || keys[i].record & ImageKey::isEntryInsideFunction)
                    continue;
                for (auto& check : builder.reportableSitesOf(typedFunctions[i]->index).parameterCheckSites) {
                    definitions.append({ check.typedFunction, sourceNumbers[i][check.definition.source], check.definition.line, check.definition.column });
                    rows.append({ typedFunctions[i]->index, check.site, definitions.last() });
                }
            }
            std::ranges::sort(rows);
            rows.shrink(std::ranges::unique(rows).begin() - rows.begin());
            std::ranges::sort(definitions);
            definitions.shrink(std::ranges::unique(definitions).begin() - definitions.begin());
            numberOfParameterChecks = rows.size();
            auto appendNumber = [&](size_t number) {
                for (;; number >>= 7) {
                    parameterChecks.append(static_cast<uint8_t>(number < 0x80 ? number : number | 0x80));
                    if (number < 0x80)
                        break;
                }
            };
            appendNumber(definitions.size());
            for (auto& definition : definitions) {
                for (uint32_t word : definition)
                    appendNumber(word);
            }
            uint32_t previousFunction = 0;
            uint32_t previousSite = 0;
            for (auto& [function, site, definition] : rows) {
                if (function != previousFunction)
                    previousSite = 0;
                appendNumber(function - previousFunction);
                appendNumber(site - previousSite);
                appendNumber(std::ranges::lower_bound(definitions, definition) - definitions.begin());
                previousFunction = function;
                previousSite = site;
            }
        }
        {
            Vector<uint32_t> sorted(builder.m_sourceNames.size(), [](size_t index) {
                return static_cast<uint32_t>(index);
            });
            auto isRelative = [&](uint32_t index) {
                auto name = builder.m_sourceNames[index].span();
                return !name.empty() && name.front() == ProgramData::relativeSourceNameMarker;
            };
            auto textOf = [&](uint32_t index) {
                return builder.m_sourceNames[index].span().subspan(isRelative(index));
            };
            std::ranges::sort(sorted, [&](uint32_t a, uint32_t b) {
                return std::ranges::lexicographical_compare(textOf(a), textOf(b));
            });
            sourceNameOffsets.fill(0, sorted.size());
            size_t firstBytesOffset = 0;
            size_t firstRank = 0;
            for (size_t rank = 0; rank < sorted.size(); ++rank) {
                sourceNameOffsets[sorted[rank]] = sourceNameBytes.size();
                auto name = textOf(sorted[rank]);
                RELEASE_ASSERT(name.size() < 0x8000);
                size_t shared = 0;
                if (rank && rank - firstRank < sourceNameRestartInterval && sourceNameBytes.size() - firstBytesOffset < 60000) {
                    auto first = textOf(sorted[firstRank]);
                    shared = std::ranges::mismatch(name, first).in1 - name.begin();
                } else {
                    firstRank = rank;
                    firstBytesOffset = sourceNameBytes.size() + sizeof(SourceNameHeader);
                }
                SourceNameHeader head { static_cast<uint16_t>(shared ? sourceNameBytes.size() - firstBytesOffset : 0), static_cast<uint16_t>(shared | (isRelative(sorted[rank]) ? 0x8000 : 0)), static_cast<uint16_t>((name.size() - shared) | (charactersAreAllASCII(name) ? 0x8000 : 0)) };
                sourceNameBytes.append(asBytes(singleElementSpan(head)));
                sourceNameBytes.append(name.subspan(shared));
            }
        }
    });
    builder.fillTypedInfos(typedFunctions.span(), typedPositions.span(), precomputed.span());
    for (size_t i = 0; i < keys.size(); ++i) {
        auto& key = keys[i];
        if (key.record & ImageKey::isEntryInsideFunction) {
            while (typedEntries.size() <= key.start)
                typedEntries.append(0);
            typedEntries[key.start] = precomputed[i].closureWord;
            continue;
        }
        if (typedFunctions[i]) {
            if (key.kind != ImageKey::typedFunction)
                continue;
            while (typedEntries.size() <= key.start)
                typedEntries.append(0);
            typedEntries[key.start] = precomputed[i].closureWord;
        }
    }
    sourceNameThread->waitForCompletion();
    endStep("the typed infos and entries"_s);
    while (builder.m_out.size() % sizeof(uint64_t))
        builder.m_out.append(0);
    if (!typedEntries.isEmpty()) {
        auto range = closureWordRangeIn(codeImage);
        RELEASE_ASSERT(range.omittedClosureWordCount <= range.firstClosureFunction && range.firstClosureFunction <= range.endOfClosureFunctions);
        while (typedEntries.size() < range.endOfClosureFunctions)
            typedEntries.append(0);
        typedEntries.shrink(range.endOfClosureFunctions);
        typedEntries.removeAt(range.firstClosureFunction - range.omittedClosureWordCount, range.omittedClosureWordCount);
        header.firstClosureFunction = range.firstClosureFunction;
        header.omittedClosureWordCount = range.omittedClosureWordCount;
    }
    header.typedEntriesOffset = builder.append(typedEntries);
    header.parameterChecksOffset = builder.append(parameterChecks);
    header.numberOfParameterChecks = numberOfParameterChecks;
    header.numberOfTypedEntries = typedEntries.size();
    header.infosOffset = builder.append(builder.m_infos);
    header.functionIndexAndFlagsOffset = builder.append(builder.m_indexAndFlags);
    header.numberOfFunctions = builder.m_infos.size();
    header.numberOfStates = 1;
    for (auto& info : builder.m_infos)
        header.numberOfStates = std::max<uint32_t>(header.numberOfStates, info.stateNumber + 1);
    {
        FunctionInfo info { };
        info.stateNumber = 1;
        uint32_t word;
        memcpy(&word, reinterpret_cast<const uint8_t*>(&info) + FunctionInfo::offsetOfBitsAndStateNumber(), sizeof(word));
        RELEASE_ASSERT(word == 1u << FunctionInfo::stateNumberShift);
        ImageFunction function { };
        function.numSlots = 0x1234;
        uint16_t numSlots;
        memcpy(&numSlots, reinterpret_cast<const uint8_t*>(&function + 1) + ImageFunction::offsetOfNumSlotsFromSites(), sizeof(numSlots));
        RELEASE_ASSERT(numSlots == 0x1234);
    }
    header.functionMetadataTableOffset = builder.append(builder.m_functionMetadataOffsets);
    header.numberOfIdentifiers = builder.m_identifiers.size();
    header.numberOfConstants = builder.m_constants.size();
    header.numberOfTopLevelCodes = builder.m_topLevelCodes.size();
    header.sourceNamesOffset = builder.append(sourceNameOffsets);
    builder.m_out.appendVector(sourceNameBytes);
    header.numberOfSources = builder.m_sourceNames.size();
    endStep("infos and metadata"_s);
    if (image->keysAreOmitted()) {
        auto isLookedUp = [&](const ImageKey& key) {
            return key.record && key.kind != std::numeric_limits<uint32_t>::max() && !(key.kind & (ImageKey::typedBody | ImageKey::typedFunction));
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
    endStep("the keys"_s);
    header.size = safeCast<uint32_t>(builder.m_out.size());
    memcpy(builder.m_out.mutableSpan().data(), &header, sizeof(header));
    if (Options::verboseAOTCompilation()) [[unlikely]]
        dataLogLn("AOT: program data: ", header.size, " bytes: strings ", header.stringsSize, ", objects ", header.objectsSize, "; ", header.numberOfFunctions, " functions, ", header.numberOfIdentifiers, " identifiers, ", header.numberOfConstants, " constants, ", header.numberOfSources, " sources");
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

const ProgramModule* ProgramData::moduleWithEntryOffset(uint32_t entryOffset) const
{
    auto all = modules();
    size_t index = std::ranges::lower_bound(all, entryOffset, { }, &ProgramModule::entryOffset) - all.begin();
    return index != all.size() && all[index].entryOffset == entryOffset ? &all[index] : nullptr;
}

String ProgramData::sourceName(uint32_t source) const
{
    RELEASE_ASSERT(source && source <= numberOfSources);
    auto* places = at<uint32_t>(sourceNamesOffset);
    auto* record = reinterpret_cast<const char8_t*>(places + numberOfSources) + places[source - 1];
    SourceNameHeader head;
    memcpy(&head, record, sizeof(head));
    size_t rest = head.restAndIsASCII & 0x7fff;
    std::span shared { record - head.firstBytesDistance, static_cast<size_t>(head.sharedAndIsRelative & 0x7fff) };
    std::span own { record + sizeof(head), rest };
    if (head.restAndIsASCII & 0x8000) [[likely]] {
        std::span<Latin1Character> characters;
        String name = StringImpl::createUninitialized(shared.size() + rest, characters);
        memcpySpan(characters, byteCast<Latin1Character>(shared));
        memcpySpan(characters.subspan(shared.size()), byteCast<Latin1Character>(own));
        return name;
    }
    Vector<char8_t, 256> name;
    name.append(shared);
    name.append(own);
    return String::fromUTF8(name.span());
}

String ProgramData::sourceNameInFrames(uint32_t source) const
{
    RELEASE_ASSERT(source && source <= numberOfSources);
    auto* places = at<uint32_t>(sourceNamesOffset);
    auto* record = reinterpret_cast<const char8_t*>(places + numberOfSources) + places[source - 1];
    SourceNameHeader head;
    memcpy(&head, record, sizeof(head));
    if (!(head.sharedAndIsRelative & 0x8000))
        return sourceName(source);
    std::span shared { record - head.firstBytesDistance, static_cast<size_t>(head.sharedAndIsRelative & 0x7fff) };
    std::span own { record + sizeof(head), static_cast<size_t>(head.restAndIsASCII & 0x7fff) };
    constexpr auto root = "/$bunfs/root/"_s;
    if ((head.restAndIsASCII & 0x8000) && (shared.empty() ? own.front() : shared.front()) != '.') [[likely]] {
        std::span<Latin1Character> characters;
        String name = StringImpl::createUninitialized(root.length() + shared.size() + own.size(), characters);
        memcpySpan(characters, root.span8());
        memcpySpan(characters.subspan(root.length()), byteCast<Latin1Character>(shared));
        memcpySpan(characters.subspan(root.length() + shared.size()), byteCast<Latin1Character>(own));
        return name;
    }
    Vector<char8_t, 256> name;
    name.append(byteCast<char8_t>(root.span8()));
    size_t start = name.size();
    name.append(shared);
    name.append(own);
    size_t levelsAbove = 0;
    while (spanHasPrefix(name.span().subspan(start + levelsAbove * 3), byteCast<char8_t>("../"_s.span8())))
        ++levelsAbove;
    if (levelsAbove) [[unlikely]] {
        Vector<char8_t, 256> inside;
        inside.append(name.span().first(start));
        for (size_t level = 0; level < levelsAbove; ++level)
            inside.append(byteCast<char8_t>("_.._/"_s.span8()));
        inside.append(name.span().subspan(start + levelsAbove * 3));
        name = WTF::move(inside);
    }
    if (head.restAndIsASCII & 0x8000) [[likely]]
        return String(byteCast<Latin1Character>(name.span()));
    return String::fromUTF8(name.span());
}

bool ProgramData::isSourceNameEmpty(uint32_t source) const
{
    RELEASE_ASSERT(source && source <= numberOfSources);
    auto* places = at<uint32_t>(sourceNamesOffset);
    SourceNameHeader head;
    memcpy(&head, reinterpret_cast<const char8_t*>(places + numberOfSources) + places[source - 1], sizeof(head));
    return !(head.sharedAndIsRelative & 0x7fff) && !(head.restAndIsASCII & 0x7fff);
}

std::span<const uint8_t> ProgramData::typedFunctionHandlers(uint32_t functionIndex) const
{
    uint32_t word = functionMetadataOffsets()[functionIndex];
    if (!FunctionMetadata::isTypedFunctionWithHandlers(word))
        return { };
    const uint8_t* bytes = at<uint8_t>(word >> 2);
    size_t size = 0;
    for (unsigned shift = 0;; shift += 7) {
        uint8_t byte = *bytes++;
        size |= static_cast<size_t>(byte & 0x7f) << shift;
        if (!(byte & 0x80))
            return { bytes, size };
    }
}

const uint8_t* ProgramData::typedFunctionPositions(uint32_t functionIndex) const
{
    uint32_t word = functionMetadataOffsets()[functionIndex];
    if (word & FunctionMetadata::isTypedFunctionPositions)
        return at<uint8_t>(word >> 1);
    if (!FunctionMetadata::isTypedFunctionWithHandlers(word))
        return nullptr;
    auto handlers = typedFunctionHandlers(functionIndex);
    return handlers.data() + handlers.size();
}

auto ProgramData::parameterCheckAt(uint32_t functionIndex, uint32_t site) const -> std::optional<ParameterCheck>
{
    const uint8_t* bytes = at<uint8_t>(parameterChecksOffset);
    auto read = [&] {
        uint32_t number = 0;
        for (unsigned shift = 0;; shift += 7) {
            uint8_t byte = *bytes++;
            number |= static_cast<uint32_t>(byte & 0x7f) << shift;
            if (!(byte & 0x80))
                return number;
        }
    };
    if (!numberOfParameterChecks)
        return std::nullopt;
    uint32_t numberOfDefinitions = read();
    const uint8_t* definitions = bytes;
    for (uint32_t remaining = numberOfDefinitions * 4; remaining--;)
        read();
    uint32_t function = 0;
    uint32_t rowSite = 0;
    for (uint32_t remaining = numberOfParameterChecks; remaining-- && function <= functionIndex;) {
        uint32_t step = read();
        if (step)
            rowSite = 0;
        function += step;
        rowSite += read();
        uint32_t definition = read();
        if (function != functionIndex || rowSite != site)
            continue;
        bytes = definitions;
        for (uint32_t skipped = definition * 4; skipped--;)
            read();
        ParameterCheck check;
        check.typedFunction = read();
        check.source = read();
        check.line = read();
        check.column = read();
        return check;
    }
    return std::nullopt;
}

Vector<uint32_t> ProgramData::moduleEntryOffsets() const
{
    Vector<uint32_t> result;
    for (auto& module : modules()) {
        if (module.hasTopLevelCode() || module.isCompiledModule())
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
    bool hasVisitedEverythingInThisCollection { false };
    SegmentedVector<Identifier, 64> overflowIdentifiers;
    UncheckedKeyHashMap<uint32_t, uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> overflowIdentifierPositions;
    Vector<RefPtr<SourceProvider>> providers;
    UncheckedKeyHashMap<uint32_t, FixedVector<UnlinkedStringJumpTable>> stringSwitchJumpTables;
    UncheckedKeyHashMap<uint32_t, FixedVector<IdentifierSet>> identifierSets;
};

template<typename T> static T* zeroedTable(size_t count)
{
    static_assert(std::is_trivially_default_constructible_v<T>, "this table is zeroed pages and its entries are never constructed, so default member initializers would be ignored");
    return static_cast<T*>(OSAllocator::reserveAndCommit(roundUpToMultipleOf(WTF::pageSize(), std::max<size_t>(count, 1) * sizeof(T)), OSAllocator::FastMallocPages));
}

template<typename T> static void freeTable(T* table, size_t count)
{
    OSAllocator::decommitAndRelease(table, roundUpToMultipleOf(WTF::pageSize(), std::max<size_t>(count, 1) * sizeof(T)));
}

static size_t identifierCapacity(const ProgramData& data)
{
    return std::min<size_t>(data.numberOfIdentifiers, UINT16_MAX) + 1;
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
    , m_identifiers(zeroedTable<UniquedStringImpl*>(identifierCapacity(m_data)))
    , m_identifierPositions(zeroedTable<uint16_t>(m_data.numberOfIdentifiers))
    , m_impl(makeUnique<Impl>())
{
    m_impl->providers.grow(m_data.numberOfModules);
    constexpr uint32_t initialCapacity = 1024;
    m_constantValues = static_cast<EncodedJSValue*>(fastZeroedMalloc(initialCapacity * sizeof(EncodedJSValue)));
    m_constantKeys = static_cast<uint32_t*>(fastZeroedMalloc(initialCapacity * sizeof(uint32_t)));
    m_constantMask = initialCapacity - 1;
}

VMProgram::~VMProgram()
{
    Typed::destroy(typedFunctionTables);
    for (uint32_t place = 1; place <= m_positionedIdentifierCount; ++place)
        m_identifiers[place]->deref();
    fastFree(m_constantValues);
    fastFree(m_constantKeys);
    freeTable(m_identifiers, identifierCapacity(m_data));
    freeTable(m_identifierPositions, m_data.numberOfIdentifiers);
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

NEVER_INLINE UniquedStringImpl* VMProgram::identifierSlow(uint32_t index)
{
    uint16_t place = m_identifierPositions[index];
    if (place == identifierStoredElsewhere) [[unlikely]]
        return m_impl->overflowIdentifiers[m_impl->overflowIdentifierPositions.get(index)].impl();
    if (place)
        return m_identifiers[place];
    Identifier identifier = Typed::tryCreateIdentifier(m_vm, index);
    if (identifier.isNull())
        identifier = ensureDecoder(m_impl->decoder, m_vm, m_data, strings()).identifier(index);
    RELEASE_ASSERT(!identifier.isNull());
    return retainIdentifier(index, WTF::move(identifier)).impl();
}

const Identifier& VMProgram::retainIdentifier(uint32_t index, Identifier&& identifier)
{
    ASSERT(!m_identifierPositions[index]);
    if (m_positionedIdentifierCount + 1 == identifierStoredElsewhere) [[unlikely]] {
        m_impl->overflowIdentifierPositions.add(index, m_impl->overflowIdentifiers.size());
        m_impl->overflowIdentifiers.append(WTF::move(identifier));
        m_identifierPositions[index] = identifierStoredElsewhere;
        return m_impl->overflowIdentifiers.last();
    }
    uint16_t place = ++m_positionedIdentifierCount;
    m_identifiers[place] = &Ref { *identifier.impl() }.leakRef();
    WTF::storeStoreFence();
    m_identifierPositions[index] = place;
    return *reinterpret_cast<const Identifier*>(&m_identifiers[place]);
}

const Identifier* VMProgram::tryGetIdentifierConcurrently(uint32_t index) const
{
    RELEASE_ASSERT(index < m_data.numberOfIdentifiers);
    uint16_t place = m_identifierPositions[index];
    if (index && (!place || place == identifierStoredElsewhere))
        return nullptr;
    WTF::loadLoadFence();
    return reinterpret_cast<const Identifier*>(&m_identifiers[place]);
}

NEVER_INLINE const Identifier& VMProgram::identifierAsIdentifierSlow(uint32_t index)
{
    identifierSlow(index);
    uint16_t place = m_identifierPositions[index];
    if (place == identifierStoredElsewhere) [[unlikely]]
        return m_impl->overflowIdentifiers[m_impl->overflowIdentifierPositions.get(index)];
    return *reinterpret_cast<const Identifier*>(&m_identifiers[place]);
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
    if (!module || !module->hasTopLevelCode() || key.flagsBitsWithoutCodeGenerationMode() != module->keyFlags || !key.name().isEmpty() || key.functionConstructorParametersEndPosition() != -1)
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
    bool visitsEverything = !std::is_same_v<Visitor, SlotVisitor> || (scope == CollectionScope::Full && !std::exchange(m_impl->hasVisitedEverythingInThisCollection, true));
    if (!visitsEverything) {
        for (JSCell* cell : m_impl->createdSinceLastCollection)
            visitor.appendUnbarriered(cell);
        return;
    }
    for (uint32_t i = 0; i <= m_constantMask; ++i) {
        if (JSValue value = JSValue::decode(m_constantValues[i]); m_constantKeys[i] && value.isCell())
            visitor.appendUnbarriered(value.asCell());
    }
}
template void VMProgram::visit(AbstractSlotVisitor&, CollectionScope);
template void VMProgram::visit(SlotVisitor&, CollectionScope);

void VMProgram::didFinishCollection()
{
    m_impl->createdSinceLastCollection.clear();
    m_impl->hasVisitedEverythingInThisCollection = false;
    if (Options::useAOTOperationCounters() && m_vm.m_aotRuntimeTable && m_vm.heap.collectionScope().value_or(CollectionScope::Full) == CollectionScope::Full) [[unlikely]] {
        RuntimeTable& table = *m_vm.m_aotRuntimeTable;
        table.clearLiveBytes();
        m_vm.heap.objectSpace().forEachSubspace([&](Subspace& subspace) {
            subspace.forEachDirectory([&](BlockDirectory& directory) {
                size_t bytes = 0;
                directory.forEachBlock([&](MarkedBlock::Handle* handle) {
                    bytes += handle->size();
                });
                table.setLiveBytes(subspace, directory.cellSize(), bytes);
            });
            return IterationStatus::Continue;
        });
        for (PreciseAllocation* allocation : m_vm.heap.objectSpace().preciseAllocations()) {
            if (allocation->isMarked())
                table.setLiveBytes(*allocation->subspace(), 0, allocation->cellSize());
        }
        table.writeOperationCounts();
    }
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
        dataLog("AOT: made so far: ", m_positionedIdentifierCount + m_impl->overflowIdentifiers.size(), " of ", m_data.numberOfIdentifiers, " identifiers, ", m_numberOfMaterializedConstants, " of ", m_data.numberOfConstants, " constants:");
        for (auto& [info, entry] : byClass)
            dataLog(" ", info->className, " ", entry.first, " (", entry.second, " bytes)");
        dataLogLn("; ", m_impl->stringSwitchJumpTables.size(), " + ", m_impl->identifierSets.size(), " tables of functions");
        if (m_vm.heap.collectionScope().value_or(CollectionScope::Full) == CollectionScope::Full) {
            dataLogLn("CENSUS begin");
            m_vm.heap.objectSpace().forEachSubspace([&](Subspace& subspace) {
                subspace.forEachDirectory([&](BlockDirectory& directory) {
                size_t blocks = 0;
                size_t capacity = 0;
                size_t marked = 0;
                size_t emptyBlocks = 0;
                size_t blocksUnderAQuarter = 0;
                directory.forEachBlock([&](MarkedBlock::Handle* handle) {
                    size_t cells = handle->cellsPerBlock();
                    size_t markedHere = handle->markCount();
                    ++blocks;
                    capacity += cells;
                    marked += markedHere;
                    emptyBlocks += !markedHere;
                    blocksUnderAQuarter += markedHere * 4 < cells;
                });
                if (blocks)
                    dataLogLn("BLOCKS\t", directory.subspace()->name(), "\t", directory.cellSize(), "\t", blocks, "\t", capacity, "\t", marked, "\t", emptyBlocks, "\t", blocksUnderAQuarter);
                });
                return IterationStatus::Continue;
            });
            size_t preciseAllocations = 0;
            size_t preciseBytes = 0;
            for (PreciseAllocation* allocation : m_vm.heap.objectSpace().preciseAllocations()) {
                if (allocation->isMarked()) {
                    ++preciseAllocations;
                    preciseBytes += allocation->cellSize();
                }
            }
            dataLogLn("PRECISE\t", preciseAllocations, "\t", preciseBytes);
        }
    }
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
