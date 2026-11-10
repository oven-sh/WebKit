/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTClosureWord.h"
#include "AOTCompileOptions.h"
#include "AOTLimits.h"
#include "AOTRuntimeTable.h"
#include "AOTStubFrames.h"
#include "AOTVarint.h"
#include "AOTVector.h"
#include "CPU.h"
#include "CPUFeatureSet.h"
#include "StackAlignment.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <wtf/OptionSet.h>
#include <wtf/PageBlock.h>
#include <wtf/StdLibExtras.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

static constexpr uint64_t imageMagic = 0x3130544f414e5542ULL;
static constexpr uint32_t imageFormatVersion = 4;
static constexpr size_t imagePageSize = std::max<size_t>(CeilingOnPageSize, 16 * KB);
static constexpr size_t imageFunctionAlignment = isX86() ? 2 : sizeof(uint32_t);
static constexpr size_t imageStubsAlignment = 16;
static constexpr size_t imageSectionAlignment = 16;
static constexpr size_t imageConstantsAlignment = 16;
static constexpr size_t imageFFIDirectoryAlignment = 4;
constexpr uint32_t alignmentOfConstant(size_t size) { return static_cast<uint32_t>(std::min(imageConstantsAlignment, size_t { 1 } << std::countr_zero(size))); }
static_assert(!(imagePageSize % imageSectionAlignment));

static constexpr size_t sizeOfNearCall = isX86_64() ? 5 : sizeof(uint32_t);
static constexpr size_t sizeOfJump = isX86_64() ? 5 : sizeof(uint32_t);
static constexpr size_t codeOffsetUnit = isX86_64() ? 1 : sizeof(uint32_t);

#define FOR_EACH_AOT_IMAGE_SECTION(macro) \
    macro(Functions, ImageFunction) \
    macro(FunctionStarts, uint32_t) \
    macro(CodeGranules, uint32_t) \
    macro(InnerEntries, ImageInnerEntry) \
    macro(Frames, ImageFrame) \
    macro(FrameMaps, uint32_t) \
    macro(ClosureWords, uint64_t) \
    macro(Positions, uint8_t) \
    macro(FaultSites, uint8_t) \
    macro(SideTables, uint8_t) \
    macro(SharedCodeCopies, uint32_t) \
    macro(Strings, ImageStringWord) \
    macro(LongStrings, uint32_t) \
    macro(Text, uint8_t) \
    macro(Identifiers, ImageIdentifierWord) \
    macro(StringConstants, uint32_t) \
    macro(NamesOfSites, uint16_t) \
    macro(StringHeaders, ImageStringHeaderOrRecord) \
    macro(RegExps, ImageRegExp) \
    macro(RegExpHashes, ImageRegExpHash) \
    macro(RegExpDetails, uint8_t) \
    macro(WebAssemblyDirectory, uint8_t) \
    macro(ProgramTables, uint8_t) \
    macro(StubAddresses, StubAddressInImage) \
    macro(StubReturnSites, StubReturnSite) \
    macro(MethodTable, uint32_t) \
    macro(SlotHandlers, uint32_t)

enum class ImageSection : uint8_t {
#define AOT_NAME_OF_SECTION(name, Element) name,
    FOR_EACH_AOT_IMAGE_SECTION(AOT_NAME_OF_SECTION)
#undef AOT_NAME_OF_SECTION
};
static constexpr unsigned numberOfImageSections = static_cast<unsigned>(ImageSection::SlotHandlers) + 1;

struct ImageSectionRange {
    uint32_t offset;
    uint32_t size;
};
static_assert(defaultLimitValue(LimitID::TableBytes, hostArchitecture()) == 1ull << (8 * sizeof(ImageSectionRange::offset)));

struct ImageHeapOptions {
    double sizeClassProgression;
    uint32_t markedBlockCageShift;
    uint32_t reserved;
};

struct ImageCounts {
    uint32_t functions;
    uint32_t maxRegExpOutputSize;
};

struct ImageCodeRanges {
    uint32_t functionsEnd;
    uint32_t codeGranuleShift;
    uint32_t sharedCodeSize;
    uint32_t webAssemblyCodeOffset;
    uint32_t webAssemblyCodeSize;
    uint32_t ffiCodeOffset;
    uint32_t ffiCodeSize;
    uint32_t regExpCodeOffset;
    uint32_t constantsOffset;
    uint32_t constantsSize;
};

enum class ImageFlag : uint32_t {
    IsObfuscated = 1 << 0,
    AdmitsUnknownJavaScript = 1 << 1,
    LoadsModulesByName = 1 << 2,
    AllowsLibrarySource = 1 << 3,
    ShowsWaysBetweenModuleObjects = 1 << 4,
    LoadsCodeOutsideImage = 1 << 5,
    IsBuiltWithLibrary = 1 << 6,
    ShowsEntriesOfRequireCache = 1 << 7,
    RunsEntryPointsAgain = 1 << 8,
    LetsGlobalObjectGo = 1 << 9,
    SetsMainModulePath = 1 << 10,
    RefusesOriginalsBesideMocks = 1 << 12,
};

struct ImageNumberRanges {
    uint32_t lastShape;
    uint32_t endOfClosureNumbers;
    uint32_t endOfIdentifiers;
    uint32_t endOfStringHeaders;
};

struct ImageHeader {
    bool has(ImageFlag flag) const { return flags & static_cast<uint32_t>(flag); }

    uint64_t magic;
    uint64_t stamp;
    std::array<uint8_t, 16> identity;
    uint64_t size;
    uint64_t codeOffset;
    uint64_t codeSize;
    uint64_t address;
    uint64_t executableSize;
    uint32_t formatVersion;
    uint32_t reserved;
    RecordedOptions options;
    ImageHeapOptions heap;
    ImageSectionRange sections[numberOfImageSections];
    ImageCounts counts;
    ImageCodeRanges code;
    uint32_t flags;
    uint32_t entriesUsed[(numberOfEntries + 31) / 32];
    uint32_t singleReturnSites[numberOfStubFrameKinds];
    uint32_t numberOfStubsFoundByKey;
    ImageNumberRanges numbers;
};
static_assert(std::is_trivially_copyable_v<ImageHeader> && std::is_standard_layout_v<ImageHeader>);
static_assert(!(offsetof(ImageHeader, sections) % 4));
static_assert(!offsetof(ImageHeader, magic) && offsetof(ImageHeader, stamp) == 8 && offsetof(ImageHeader, identity) == 16);
static_assert(offsetof(ImageHeader, size) == 32 && offsetof(ImageHeader, codeOffset) == 40 && offsetof(ImageHeader, codeSize) == 48, "Bun's test/aot/cli/compiled-image.ts and src/standalone_graph/executable_file.rs read these members by offset");
static_assert(static_cast<unsigned>(ImageSection::ProgramTables) == 22, "Bun's test/aot/cli/compiled-image.ts lists this section");
static_assert(offsetof(ImageHeader, address) == 56, "Bun's writers of executables list this member for the loader");
static_assert(offsetof(ImageHeader, executableSize) == 64, "Bun's writer of ELF executables reads this member by offset");
static_assert(offsetof(ImageHeader, formatVersion) == 72 && offsetof(ImageHeader, options) == 80, "whoever reads an image without this header finds the members in front of `options` by offset: they stay where they are");
static_assert(offsetof(ImageHeader, sections) == 112 && numberOfImageSections == 27 && offsetof(ImageHeader, code) + offsetof(ImageCodeRanges, ffiCodeSize) == 360 && offsetof(ImageHeader, code) + offsetof(ImageCodeRanges, constantsOffset) == 368 && offsetof(ImageHeader, code) + offsetof(ImageCodeRanges, constantsSize) == 372 && offsetof(ImageHeader, flags) == 376, "Bun's test/aot/cli/compiled-image.ts reads these members by offset");

JS_EXPORT_PRIVATE uint64_t imageStamp(CPUFeatureSet, const std::array<uint8_t, 16>& identity, Wire::ImageForm = imageFormOfThisRuntime());

enum class ImageFunctionFlag : uint8_t {
    HasInlineFrames = 1 << 0,
    HasHandlers = 1 << 1,
    HasFrameMaps = 1 << 2,
    HasCallSites = 1 << 3,
    HasPositions = 1 << 4,
    SpillsArgumentRegisters = 1 << 5,
    HasConstructedCallees = 1 << 7,
};

struct ImageFunction {
    static constexpr unsigned numberOfFunctionBits = 24;
    static constexpr unsigned flagsShift = numberOfFunctionBits;
    static constexpr uint32_t functionMask = (1u << numberOfFunctionBits) - 1;

    static ImageFunction create(uint32_t sideTables, uint32_t function, OptionSet<ImageFunctionFlag> flags)
    {
        RELEASE_ASSERT(function <= functionMask);
        return { sideTables, function | static_cast<uint32_t>(flags.toRaw()) << flagsShift };
    }

    uint32_t function() const { return functionAndFlags & functionMask; }
    OptionSet<ImageFunctionFlag> flags() const { return OptionSet<ImageFunctionFlag>::fromRaw(static_cast<uint8_t>(functionAndFlags >> flagsShift)); }
    bool has(ImageFunctionFlag flag) const { return flags().contains(flag); }

    uint32_t sideTables;
    uint32_t functionAndFlags;
};
static_assert(sizeof(ImageFunction) == 8 && std::has_unique_object_representations_v<ImageFunction>);
static_assert(defaultLimitValue(LimitID::ImageFunctions, hostArchitecture()) <= 1ull << ImageFunction::numberOfFunctionBits);
static_assert(defaultLimitValue(LimitID::ImageClosureFunctions, hostArchitecture()) == ClosureWord::firstReservedFunction);
static_assert(defaultLimitValue(LimitID::CodeBytes, hostArchitecture()) <= ClosureWord::maxSizeOfCode);

struct ImageFrame {
    static constexpr unsigned maxCalleeSavesStart = (1u << 6) - 1;
    static constexpr unsigned unitsAboveFrameRecordShift = 6;
    static constexpr unsigned maxUnitsAboveFrameRecord = (1u << 5) - 1;
    static constexpr uint16_t hasNoFrameRecordBit = 1u << 15;
    static constexpr uint16_t liesBelowSpilledArgumentsBit = 1u << 14;
    static constexpr uint16_t isEmptyBit = 1u << 13;
    static constexpr ImageFrame empty() { return { 0, hasNoFrameRecordBit | isEmptyBit, 0 }; }
    static constexpr unsigned firstEncodedGPR = isARM64() ? 16 : 0;
    static constexpr unsigned firstEncodedFPR = isARM64() ? 32 : 16;

    static uint64_t unpackRegisters(uint32_t packed) { return static_cast<uint64_t>(packed & 0xffff) << firstEncodedGPR | static_cast<uint64_t>(packed >> 16) << firstEncodedFPR; }
    static uint32_t packRegisters(uint64_t mask)
    {
        uint32_t packed = static_cast<uint32_t>(mask >> firstEncodedGPR & 0xffff) | static_cast<uint32_t>(mask >> firstEncodedFPR & 0xffff) << 16;
        RELEASE_ASSERT(unpackRegisters(packed) == mask);
        return packed;
    }

    unsigned frameSizeInBytes() const { return frameSizeInUnits * stackAlignmentBytes(); }
    bool hasFrameRecord() const { return !(calleeSavesStart & hasNoFrameRecordBit); }
    unsigned startOfCalleeSaves() const { return calleeSavesStart & maxCalleeSavesStart; }
    unsigned bytesAboveFrameRecord() const { return (calleeSavesStart >> unitsAboveFrameRecordShift & maxUnitsAboveFrameRecord) * stackAlignmentBytes(); }
    bool liesBelowSpilledArguments() const { return calleeSavesStart & liesBelowSpilledArgumentsBit; }
    bool isEmpty() const { return calleeSavesStart & isEmptyBit; }
    uint64_t bits() const { return static_cast<uint64_t>(calleeSaveRegisters) << 32 | static_cast<uint64_t>(calleeSavesStart) << 16 | frameSizeInUnits; }

    uint32_t calleeSaveRegisters;
    uint16_t calleeSavesStart;
    uint16_t frameSizeInUnits;
};
static_assert(sizeof(ImageFrame) == 8);

struct ImageInnerEntry {
    uint32_t codeOffset;
    uint32_t function;
};
static_assert(sizeof(ImageInnerEntry) == 8);

struct StubAddressInImage {
    uint64_t key;
    uint32_t offset;
    uint32_t usesCallConvention;
};
static_assert(sizeof(StubAddressInImage) == 16);

static constexpr uint32_t offsetOfAbsentStub = 0xffffffffu;

struct SideTableInputs {
    OptionSet<ImageFunctionFlag> flags() const;

    std::span<const uint8_t> frameRanges;
    uint32_t frame { 0 };
    std::optional<std::pair<uint32_t, uint32_t>> frameMapRange;
    std::span<const uint8_t> handlers;
    std::span<const std::pair<uint32_t, uint32_t>> constructedCallees;
    std::span<const uint8_t> callSites;
};

struct SideTableParts {
    static SideTableParts read(const ImageFunction&, const uint8_t* sideTables);
    static void append(Vector<uint8_t>& stream, const SideTableInputs&);
    static void appendFrameChange(Vector<uint8_t>& bytes, uint32_t previousChange, uint32_t change);

    bool hasFrameAt(uint32_t offset) const;
    std::optional<uint32_t> constructedCalleeAt(uint32_t site) const;

    std::span<const uint8_t> frameRanges;
    uint32_t frame { 0 };
    uint32_t firstFrameMapWord { 0 };
    uint32_t numberOfFrameMapWords { 0 };
    std::span<const uint8_t> handlers;
    std::span<const uint8_t> constructedCallees;
    const uint8_t* callSites { nullptr };
};

struct PositionIndex {
    static constexpr unsigned functionsBetween = 32;
    static constexpr size_t numberOfEntriesFor(size_t numberOfFunctions) { return (numberOfFunctions + functionsBetween - 1) / functionsBetween; }
};

inline SideTableParts SideTableParts::read(const ImageFunction& function, const uint8_t* sideTables)
{
    SideTableParts parts;
    const uint8_t* cursor = sideTables + function.sideTables;
    size_t frameRangesSize = static_cast<size_t>(readVarint(cursor));
    parts.frameRanges = unsafeMakeSpan(cursor, frameRangesSize);
    cursor += frameRangesSize;
    if (frameRangesSize)
        parts.frame = static_cast<uint32_t>(readVarint(cursor));
    if (function.has(ImageFunctionFlag::HasFrameMaps)) {
        parts.firstFrameMapWord = static_cast<uint32_t>(readVarint(cursor));
        parts.numberOfFrameMapWords = static_cast<uint32_t>(readVarint(cursor));
    }
    if (function.has(ImageFunctionFlag::HasHandlers)) {
        size_t size = static_cast<size_t>(readVarint(cursor));
        parts.handlers = unsafeMakeSpan(cursor, size);
        cursor += size;
    }
    if (function.has(ImageFunctionFlag::HasConstructedCallees)) {
        size_t size = static_cast<size_t>(readVarint(cursor));
        parts.constructedCallees = unsafeMakeSpan(cursor, size);
        cursor += size;
    }
    if (function.has(ImageFunctionFlag::HasCallSites))
        parts.callSites = cursor;
    return parts;
}

inline OptionSet<ImageFunctionFlag> SideTableInputs::flags() const
{
    OptionSet<ImageFunctionFlag> flags;
    flags.set(ImageFunctionFlag::HasFrameMaps, frameMapRange && frameMapRange->second);
    flags.set(ImageFunctionFlag::HasHandlers, !handlers.empty());
    flags.set(ImageFunctionFlag::HasConstructedCallees, !constructedCallees.empty());
    flags.set(ImageFunctionFlag::HasCallSites, !callSites.empty());
    return flags;
}

inline void SideTableParts::append(Vector<uint8_t>& stream, const SideTableInputs& inputs)
{
    OptionSet<ImageFunctionFlag> flags = inputs.flags();
    appendVarint(stream, inputs.frameRanges.size());
    stream.append(inputs.frameRanges);
    if (!inputs.frameRanges.empty())
        appendVarint(stream, inputs.frame);
    if (flags.contains(ImageFunctionFlag::HasFrameMaps)) {
        appendVarint(stream, inputs.frameMapRange->first);
        appendVarint(stream, inputs.frameMapRange->second);
    }
    if (flags.contains(ImageFunctionFlag::HasHandlers)) {
        appendVarint(stream, inputs.handlers.size());
        stream.append(inputs.handlers);
    }
    if (flags.contains(ImageFunctionFlag::HasConstructedCallees)) {
        Vector<uint8_t> rows;
        uint32_t previous = 0;
        for (auto [site, function] : inputs.constructedCallees) {
            RELEASE_ASSERT(site > previous);
            appendVarint(rows, site - std::exchange(previous, site));
            appendVarint(rows, function);
        }
        appendVarint(stream, rows.size());
        stream.appendVector(rows);
    }
    stream.append(inputs.callSites);
}

inline std::optional<uint32_t> SideTableParts::constructedCalleeAt(uint32_t site) const
{
    const uint8_t* cursor = constructedCallees.data();
    const uint8_t* end = cursor + constructedCallees.size();
    uint64_t listed = 0;
    while (cursor < end && listed < site) {
        listed += readVarint(cursor);
        uint32_t function = static_cast<uint32_t>(readVarint(cursor));
        if (listed == site)
            return function;
    }
    return std::nullopt;
}

inline void SideTableParts::appendFrameChange(Vector<uint8_t>& bytes, uint32_t previousChange, uint32_t change)
{
    RELEASE_ASSERT(previousChange <= change && !(previousChange % codeOffsetUnit) && !(change % codeOffsetUnit));
    appendVarint(bytes, (change - previousChange) / codeOffsetUnit);
}

inline bool SideTableParts::hasFrameAt(uint32_t offset) const
{
    const uint8_t* cursor = frameRanges.data();
    const uint8_t* end = cursor + frameRanges.size();
    uint64_t position = offset / codeOffsetUnit;
    uint64_t change = 0;
    bool hasFrame = false;
    while (cursor < end) {
        change += readVarint(cursor);
        if (position < change)
            return hasFrame;
        hasFrame = !hasFrame;
    }
    return hasFrame;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
