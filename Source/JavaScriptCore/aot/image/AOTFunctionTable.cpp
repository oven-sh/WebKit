/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFunctionTable.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCodeIndex.h"
#include "AOTCodeLayout.h"
#include "AOTFunctionTables.h"
#include "AOTImage.h"
#include "AOTLinkedFunctions.h"
#include "AOTSectionWriter.h"
#include "AOTThreads.h"
#include "RegisterAtOffsetList.h"
#include "RegisterSet.h"
#include <algorithm>
#include <limits>
#include <optional>
#include <wtf/HashMap.h>
#include <wtf/MathExtras.h>
#include <wtf/StdLibExtras.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

namespace {

struct FrameShape {
    uint64_t calleeSaveRegisters;
    uint16_t calleeSavesStart;
    uint16_t frameSizeInUnits;
};

struct FrameTable {
    Vector<ImageFrame> frames;
    Vector<uint32_t> frameOf;
};

} // anonymous namespace

static bool checkFunctionLimit(const LinkedFunctions& functions, const Vector<EntryAlias>& aliases, BuildContext& context)
{
    uint64_t endOfFunctionIDs = 0;
    for (const CompiledCode& code : functions)
        endOfFunctionIDs = std::max(endOfFunctionIDs, static_cast<uint64_t>(code.function.index()) + 1);
    for (const EntryAlias& alias : aliases)
        endOfFunctionIDs = std::max(endOfFunctionIDs, static_cast<uint64_t>(alias.function.index()) + 1);
    uint64_t count = std::max<uint64_t>(functions.size(), endOfFunctionIDs);
    if (!context.exceeds(LimitID::ImageFunctions, count))
        return true;
    context.diagnostics().add(UserDiagnostic { .kind = UserDiagnosticKind::TooManyFunctions, .limit = LimitID::ImageFunctions, .count = clampTo<uint32_t>(count) });
    return false;
}

static uint64_t unwindableRegisterMask()
{
    uint64_t mask = 0;
    for (const RegisterAtOffset& entry : *RegisterSet::vmCalleeSaveRegisterOffsets()) {
        if (!RegisterSet::stackRegisters().contains(entry.reg(), IgnoreVectors))
            mask |= 1ULL << entry.reg().index();
    }
    return mask;
}

static FrameShape frameShapeOf(const FrameDescription& frame)
{
    static_assert(defaultLimitValue(LimitID::MaxFrameSizeInBytes, hostArchitecture()) / stackAlignmentBytes() <= UINT16_MAX);
    RELEASE_ASSERT(!(frame.frameSizeInBytes % stackAlignmentBytes()));
    FrameShape shape { 0, 0, safeCast<uint16_t>(frame.frameSizeInBytes / stackAlignmentBytes()) };
    for (unsigned i = 0; i < frame.calleeSaveRegisters.registerCount(); ++i) {
        const RegisterAtOffset& entry = frame.calleeSaveRegisters.at(i);
        RELEASE_ASSERT(entry.reg().index() < 64 && entry.offset() == frame.calleeSaveRegisters.at(0).offset() + static_cast<ptrdiff_t>(i * sizeof(CPURegister)) && (!i || entry.reg().index() > frame.calleeSaveRegisters.at(i - 1).reg().index()));
        shape.calleeSaveRegisters |= 1ULL << entry.reg().index();
    }
    if (frame.calleeSaveRegisters.registerCount()) {
        ptrdiff_t offset = frame.calleeSaveRegisters.at(0).offset();
        RELEASE_ASSERT(offset < 0 && !(offset % static_cast<ptrdiff_t>(sizeof(CPURegister))) && -offset / static_cast<ptrdiff_t>(sizeof(CPURegister)) <= static_cast<ptrdiff_t>(ImageFrame::maxCalleeSavesStart));
        shape.calleeSavesStart = static_cast<uint16_t>(-offset / static_cast<ptrdiff_t>(sizeof(CPURegister)));
    }
    return shape;
}

static std::optional<FrameTable> collectFrames(const LinkedFunctions& functions, BuildContext& context)
{
    Vector<FrameShape> shapes(functions.size());
    forEachIndexInParallel(context.parallelLoops(), functions.size(), [&](unsigned position) {
        shapes[position] = frameShapeOf(functions[position].frame);
    });

    const uint64_t unwindableRegisters = unwindableRegisterMask();
    FrameTable result;
    result.frameOf.reserveInitialCapacity(functions.size());
    UncheckedKeyHashMap<uint64_t, uint32_t, WTF::IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> numbers;
    for (size_t position = 0; position < shapes.size(); ++position) {
        const FrameShape& shape = shapes[position];
        if (uint64_t unknownRegisters = shape.calleeSaveRegisters & ~unwindableRegisters) {
            context.diagnostics().add(InternalError { .kind = InternalErrorKind::FrameSavesUnknownRegister, .function = functions[position].function, .count = ctz(unknownRegisters) });
            return std::nullopt;
        }
        ImageFrame frame { ImageFrame::packRegisters(shape.calleeSaveRegisters), shape.calleeSavesStart, shape.frameSizeInUnits };
        result.frameOf.append(numbers.ensure(frame.bits(), [&] {
            result.frames.append(frame);
            return static_cast<uint32_t>(result.frames.size() - 1);
        }).iterator->value);
    }
    context.statistics().count(StatisticID::FrameShapesEmitted, result.frames.size());
    return result;
}

static ImageFunction encodeFunction(const CompiledCode& code, uint32_t callSites, uint32_t sideTables, uint32_t frame, bool hasPositions)
{
    const SiteDescription& sites = code.sites;
    OptionSet<ImageFunctionFlag> flags;
    flags.set(ImageFunctionFlag::HasInlineFrames, sites.hasEncodedInlineFrames());
    flags.set(ImageFunctionFlag::HasHandlers, !sites.handlers.isEmpty());
    flags.set(ImageFunctionFlag::HasFrameMaps, !sites.frameMapWords.isEmpty());
    flags.set(ImageFunctionFlag::HasCallSites, !!callSites);
    flags.set(ImageFunctionFlag::HasPositions, hasPositions && !sites.positions.isEmpty());
    return ImageFunction::create(sideTables, code.function.index(), frame, flags);
}

static Vector<ImageFunction> encodeFunctions(const FunctionTableInputs& inputs, const Vector<uint32_t>& frameOf, const ParallelLoops& loops)
{
    const LinkedFunctions& functions = inputs.functions;
    Vector<ImageFunction> records(functions.size());
    forEachIndexInParallel(loops, functions.size(), [&](unsigned position) {
        records[position] = encodeFunction(functions[position], inputs.callSiteOffsets[position], inputs.sideTableOffsets[position], frameOf[position], inputs.hasPositions);
    });
    return records;
}

static Vector<uint8_t> encodeFunctionStarts(const Vector<uint32_t>& starts)
{
    constexpr uint32_t endOfStarts = std::numeric_limits<uint32_t>::max();
    Vector<uint8_t> bytes;
    bytes.reserveInitialCapacity((starts.size() + 1) * sizeof(uint32_t));
    bytes.append(asBytes(starts.span()));
    bytes.append(asByteSpan(endOfStarts));
    return bytes;
}

static Vector<uint32_t> encodeCodeGranules(const CodeLayout& layout)
{
    const Vector<uint32_t>& bodyStarts = layout.bodyStarts;
    size_t granuleSize = static_cast<size_t>(1) << layout.codeGranuleShift;
    Vector<uint32_t> codeGranules;
    codeGranules.reserveInitialCapacity((layout.functionsEnd + granuleSize - 1) / granuleSize);
    uint32_t function = 0;
    for (size_t start = 0; start < layout.functionsEnd; start += granuleSize) {
        while (function + 1 < bodyStarts.size() && bodyStarts[function + 1] <= start)
            ++function;
        codeGranules.append(function);
    }
    return codeGranules;
}

static Vector<uint32_t> encodeFunctionsByID(const CodeIndex& index, size_t numberOfFunctions)
{
    Vector<uint32_t> positions;
    positions.reserveInitialCapacity(numberOfFunctions);
    for (uint32_t function = 0; function <= ImageFunction::functionMask && positions.size() < numberOfFunctions; ++function) {
        if (std::optional<uint32_t> position = index.indexOf(FunctionID(function)))
            positions.append(*position);
    }
    RELEASE_ASSERT(positions.size() == numberOfFunctions);
    return positions;
}

static uint32_t bodyPositionOf(const EntryAlias& alias, const CodeIndex& index)
{
    std::optional<uint32_t> position = index.indexOf(alias.body);
    RELEASE_ASSERT(position);
    return *position;
}

static void appendInnerEntries(Vector<ImageInnerEntry>& entries, const EntryAlias& alias, const CompiledCode& body, uint32_t bodyStart)
{
    switch (alias.placement) {
    case AliasPlacement::AtPrimaryEntry:
        return;
    case AliasPlacement::BeforeBody: {
        uint32_t primaryEntryOffset = body.body.primaryEntryOffset();
        RELEASE_ASSERT(*alias.innerEntryOffset() <= primaryEntryOffset);
        if (*alias.innerEntryOffset() == primaryEntryOffset)
            return;
        entries.append({ bodyStart + primaryEntryOffset, body.function.index() });
        break;
    }
    case AliasPlacement::AfterBody:
        break;
    }
    entries.append({ bodyStart + *alias.innerEntryOffset(), alias.function.index() });
}

static Vector<ImageInnerEntry> encodeInnerEntries(const FunctionTableInputs& inputs, const ParallelLoops& loops)
{
    Vector<ImageInnerEntry> entries;
    for (const EntryAlias& alias : inputs.aliases) {
        uint32_t body = bodyPositionOf(alias, inputs.index);
        appendInnerEntries(entries, alias, inputs.functions[body], inputs.layout.bodyStarts[body]);
    }
    sortAndRemoveDuplicates(loops, entries, [](const ImageInnerEntry& a, const ImageInnerEntry& b) {
        return a.codeOffset != b.codeOffset ? a.codeOffset < b.codeOffset : a.function < b.function;
    }, [](const ImageInnerEntry& a, const ImageInnerEntry& b) {
        return a.codeOffset == b.codeOffset && a.function == b.function;
    });
    return entries;
}

static Vector<uint64_t> encodeClosureWords(const FunctionTableInputs& inputs, const ParallelLoops& loops)
{
    const LinkedFunctions& functions = inputs.functions;
    const Vector<uint32_t>& starts = inputs.layout.bodyStarts;
    const ClosureFunctionRangeData& range = inputs.programTables.closureFunctions;
    std::span<const FunctionKindData> kinds = inputs.programTables.functionKinds();
    RELEASE_ASSERT(range.omittedWordCount <= range.first && range.first <= range.end && range.end <= kinds.size());
    Vector<uint64_t> words(FillWith { }, range.end, 0);
    auto setWord = [&](FunctionID function, uint32_t codeOffset, const EntryDescription& entry) {
        uint32_t index = function.index();
        if (index >= words.size())
            return;
        RELEASE_ASSERT(codeOffset && !(codeOffset % imageFunctionAlignment));
        words[index] = ClosureWord::encode({ codeOffset, entry.numberOfParameters }, index, closureWordFlags(kinds[index]));
    };
    forEachIndexInParallel(loops, functions.size(), [&](unsigned position) {
        const CompiledCode& code = functions[position];
        setWord(code.function, starts[position] + code.body.primaryEntryOffset(), code.entry);
    });
    forEachIndexInParallel(loops, inputs.aliases.size(), [&](unsigned index) {
        const EntryAlias& alias = inputs.aliases[index];
        uint32_t body = bodyPositionOf(alias, inputs.index);
        setWord(alias.function, starts[body] + alias.innerEntryOffset().value_or(functions[body].body.primaryEntryOffset()), alias.entry);
    });
    words.removeAt(range.first - range.omittedWordCount, range.omittedWordCount);
    return words;
}

EncodedFunctionTables encodeFunctionTables(const FunctionTableInputs& inputs, BuildContext& context)
{
    const LinkedFunctions& functions = inputs.functions;
    const CodeLayout& layout = inputs.layout;
    const ParallelLoops& loops = context.parallelLoops();
    RELEASE_ASSERT(layout.bodyStarts.size() == functions.size());
    RELEASE_ASSERT(inputs.callSiteOffsets.size() == functions.size() && inputs.sideTableOffsets.size() == functions.size());

    if (!checkFunctionLimit(functions, inputs.aliases, context))
        return { };

    std::optional<FrameTable> frameTable;
    EncodedFunctionTables tables;
    runTasksInParallel(loops, {
        [&] {
            frameTable = collectFrames(functions, context);
            if (frameTable)
                tables.functions = bytesOf(encodeFunctions(inputs, frameTable->frameOf, loops));
        },
        [&] { tables.functionStarts = encodeFunctionStarts(layout.bodyStarts); },
        [&] { tables.codeGranules = bytesOf(encodeCodeGranules(layout)); },
        [&] { tables.functionsByID = bytesOf(encodeFunctionsByID(inputs.index, functions.size())); },
        [&] { tables.innerEntries = bytesOf(encodeInnerEntries(inputs, loops)); },
        [&] { tables.closureWords = bytesOf(encodeClosureWords(inputs, loops)); },
    });
    if (!frameTable)
        return { };
    tables.frames = bytesOf(frameTable->frames);
    return tables;
}

std::optional<uint32_t> Image::imageFunctionOf(FunctionID id) const
{
    std::span functionsByID = section<uint32_t>(ImageSection::FunctionsByID);
    auto found = std::ranges::lower_bound(functionsByID, id.index(), { }, [&](uint32_t function) -> uint32_t {
        return this->function(function).function();
    });
    if (found == functionsByID.end() || function(*found).function() != id.index())
        return std::nullopt;
    return *found;
}

std::optional<FunctionID> Image::innerEntryContaining(const void* pc) const
{
    CodeLocation location = locate(pc);
    if (location.kind != CodeKind::Function)
        return std::nullopt;
    std::span entries = section<ImageInnerEntry>(ImageSection::InnerEntries);
    uint32_t codeOffset = static_cast<uint32_t>(static_cast<const uint8_t*>(pc) - code().data());
    auto next = std::ranges::upper_bound(entries, codeOffset, { }, &ImageInnerEntry::codeOffset);
    if (next == entries.begin())
        return std::nullopt;
    const ImageInnerEntry& entry = *std::prev(next);
    uint32_t bodyStart = codeOffset - location.offset;
    if (entry.codeOffset < bodyStart || entry.function == function(location.function).function())
        return std::nullopt;
    return FunctionID(entry.function);
}

std::span<const uint32_t> Image::frameMapWords(uint32_t function) const
{
    const ImageFunction& record = this->function(function);
    if (!record.has(ImageFunctionFlag::HasFrameMaps))
        return { };
    SideTableParts parts = SideTableParts::read(record, section<uint8_t>(ImageSection::SideTables).data());
    return section<uint32_t>(ImageSection::FrameMaps).subspan(parts.firstFrameMapWord, parts.numberOfFrameMapWords);
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
