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
#include "AOTVarint.h"
#include "CPU.h"
#include "StackAlignment.h"
#include <algorithm>
#include <array>
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
static constexpr size_t imagePageSize = std::max<size_t>(CeilingOnPageSize, 16 * KB);
static constexpr size_t imageFunctionAlignment = isX86() ? 2 : sizeof(uint32_t);
static constexpr size_t imageStubsAlignment = 16;
static constexpr size_t imageSectionAlignment = 16;
static constexpr size_t imageConstantsAlignment = 16;
static_assert(!(imagePageSize % imageSectionAlignment));

#if CPU(X86_64)
static constexpr size_t sizeOfNearCall = 5;
static constexpr size_t sizeOfJump = 5;
static constexpr size_t codeOffsetUnit = 1;
#else
static constexpr size_t sizeOfNearCall = sizeof(uint32_t);
static constexpr size_t sizeOfJump = sizeof(uint32_t);
static constexpr size_t codeOffsetUnit = sizeof(uint32_t);
#endif

enum class ImageSection : uint8_t {
    Functions,
    FunctionStarts,
    CodeGranules,
    FunctionsByID,
    InnerEntries,
    Frames,
    FrameMaps,
    ClosureWords,
    CallSites,
    FaultSites,
    SideTables,
    SharedCodeCopies,
    Strings,
    LongStrings,
    Text,
    Identifiers,
    StringConstants,
    RegExps,
    RegExpHashes,
    RegExpDetails,
    WebAssemblyDirectory,
    ProgramTables,
    StubAddresses,
    StubReturnSites,
    SlotHandlers,
};
static constexpr unsigned numberOfImageSections = 25;
static_assert(static_cast<unsigned>(ImageSection::SlotHandlers) + 1 == numberOfImageSections);

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
    uint32_t regExpCodeOffset;
    uint32_t webAssemblyCodeOffset;
    uint32_t webAssemblyCodeSize;
    uint32_t constantsOffset;
    uint32_t constantsSize;
    uint32_t ffiCodeOffset;
    uint32_t ffiCodeSize;
};

// Without AdmitsUnknownJavaScript the proofs in the code hold only while nothing but the code of the image runs, without LoadsModulesByName only while no module is found by a name,
// and without ReplacesLoaderAnswers only while a constant name loads the module it stood for in the build. The run time refuses all three.
// Without AdmitsLibraryText the image holds of a module of the host's library only what seen code reaches, so the run time lets nothing of that library run from its text.
enum class ImageFlag : uint32_t {
    IsObfuscated = 1 << 0,
    AdmitsUnknownJavaScript = 1 << 1,
    LoadsModulesByName = 1 << 2,
    ReplacesLoaderAnswers = 1 << 3,
    AdmitsLibraryText = 1 << 4,
};

// codeOffset and sections[].offset count from the header. Every member of ImageCodeRanges that is an offset counts from codeOffset.
struct ImageHeader {
    bool has(ImageFlag flag) const { return flags & static_cast<uint32_t>(flag); }
    bool isObfuscated() const { return has(ImageFlag::IsObfuscated); }
    bool admitsUnknownJavaScript() const { return has(ImageFlag::AdmitsUnknownJavaScript); }
    bool loadsModulesByName() const { return has(ImageFlag::LoadsModulesByName); }
    bool replacesLoaderAnswers() const { return has(ImageFlag::ReplacesLoaderAnswers); }
    bool admitsLibraryText() const { return has(ImageFlag::AdmitsLibraryText); }

    uint64_t magic;
    uint64_t stamp;
    std::array<uint8_t, 16> identity;
    uint64_t size;
    uint64_t codeOffset;
    uint64_t codeSize;
    RecordedOptions options;
    ImageHeapOptions heap;
    ImageSectionRange sections[numberOfImageSections];
    ImageCounts counts;
    ImageCodeRanges code;
    uint32_t flags;
    uint32_t entriesUsed[(numberOfEntries + 31) / 32];
};
static_assert(std::is_trivially_copyable_v<ImageHeader> && std::is_standard_layout_v<ImageHeader>);
static_assert(!(offsetof(ImageHeader, sections) % 4));
static_assert(!offsetof(ImageHeader, magic) && offsetof(ImageHeader, stamp) == 8 && offsetof(ImageHeader, identity) == 16);
static_assert(offsetof(ImageHeader, size) == 32 && offsetof(ImageHeader, codeOffset) == 40 && offsetof(ImageHeader, codeSize) == 48, "Bun's test/aot/cli/compiled-image.ts and test/aot/shell/expectations.py read these members by offset");

enum class ImageFunctionFlag : uint8_t {
    HasInlineFrames = 1 << 0,
    HasHandlers = 1 << 1,
    HasFrameMaps = 1 << 2,
    HasCallSites = 1 << 3,
    HasPositions = 1 << 4,
};

// function() is a FunctionID index. An image function index is a position in ImageSection::Functions.
struct ImageFunction {
    static constexpr unsigned numberOfFunctionBits = 24;
    static constexpr unsigned numberOfFrameBits = 24;
    static constexpr unsigned flagsShift = numberOfFrameBits;
    static constexpr uint32_t functionMask = (1u << numberOfFunctionBits) - 1;
    static constexpr uint32_t frameMask = (1u << numberOfFrameBits) - 1;

    static ImageFunction create(uint32_t sideTables, uint32_t function, uint32_t frame, OptionSet<ImageFunctionFlag> flags)
    {
        RELEASE_ASSERT(function <= functionMask && frame <= frameMask);
        return { sideTables, function, frame | static_cast<uint32_t>(flags.toRaw()) << flagsShift };
    }

    uint32_t function() const { return functionIndex; }
    uint32_t frame() const { return frameAndFlags & frameMask; }
    OptionSet<ImageFunctionFlag> flags() const { return OptionSet<ImageFunctionFlag>::fromRaw(static_cast<uint8_t>(frameAndFlags >> flagsShift)); }
    bool has(ImageFunctionFlag flag) const { return flags().contains(flag); }

    uint32_t sideTables;
    uint32_t functionIndex;
    uint32_t frameAndFlags;
};
static_assert(sizeof(ImageFunction) == 12 && std::has_unique_object_representations_v<ImageFunction>);
static_assert(defaultLimitValue(LimitID::ImageFunctions, hostArchitecture()) <= 1ull << ImageFunction::numberOfFunctionBits);
static_assert(defaultLimitValue(LimitID::ImageFunctions, hostArchitecture()) <= 1ull << ImageFunction::numberOfFrameBits, "no image has more frame shapes than functions");
static_assert(defaultLimitValue(LimitID::ImageFunctions, hostArchitecture()) == ClosureWord::firstReservedFunction);

struct ImageFrame {
    static constexpr unsigned maxCalleeSavesStart = (1u << 6) - 1;
#if CPU(ARM64)
    static constexpr unsigned firstEncodedGPR = 16;
    static constexpr unsigned firstEncodedFPR = 32;
#else
    static constexpr unsigned firstEncodedGPR = 0;
    static constexpr unsigned firstEncodedFPR = 16;
#endif

    static uint64_t unpackRegisters(uint32_t packed) { return static_cast<uint64_t>(packed & 0xffff) << firstEncodedGPR | static_cast<uint64_t>(packed >> 16) << firstEncodedFPR; }
    static uint32_t packRegisters(uint64_t mask)
    {
        uint32_t packed = static_cast<uint32_t>(mask >> firstEncodedGPR & 0xffff) | static_cast<uint32_t>(mask >> firstEncodedFPR & 0xffff) << 16;
        RELEASE_ASSERT(unpackRegisters(packed) == mask);
        return packed;
    }

    unsigned frameSizeInBytes() const { return frameSizeInUnits * stackAlignmentBytes(); }
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
    uint32_t reserved;
};
static_assert(sizeof(StubAddressInImage) == 16);

// SlotHandlers has an offset into the shared code for each member of PropertyCacheWords::handlers, in the order of that array.
static constexpr uint32_t offsetOfAbsentStub = 0xffffffffu;

struct SideTableInputs {
    std::span<const uint8_t> frameRanges;
    std::optional<std::pair<uint32_t, uint32_t>> frameMapRange;
    std::optional<uint32_t> callSites;
    std::span<const uint8_t> handlers;
    std::span<const uint8_t> positions;
};

struct SideTableParts {
    static SideTableParts read(const ImageFunction&, const uint8_t* sideTables);
    static void append(Vector<uint8_t>& stream, const SideTableInputs&);
    static void appendFrameRange(Vector<uint8_t>& bytes, uint32_t previousEnd, uint32_t begin, uint32_t end);

    bool hasFrameAt(uint32_t offset) const;

    std::span<const uint8_t> frameRanges;
    uint32_t firstFrameMapWord { 0 };
    uint32_t numberOfFrameMapWords { 0 };
    uint32_t callSites { 0 };
    std::span<const uint8_t> handlers;
    const uint8_t* positions { nullptr };
};

inline SideTableParts SideTableParts::read(const ImageFunction& function, const uint8_t* sideTables)
{
    SideTableParts parts;
    const uint8_t* cursor = sideTables + function.sideTables;
    size_t frameRangesSize = static_cast<size_t>(readVarint(cursor));
    parts.frameRanges = unsafeMakeSpan(cursor, frameRangesSize);
    cursor += frameRangesSize;
    if (function.has(ImageFunctionFlag::HasFrameMaps)) {
        parts.firstFrameMapWord = static_cast<uint32_t>(readVarint(cursor));
        parts.numberOfFrameMapWords = static_cast<uint32_t>(readVarint(cursor));
    }
    if (function.has(ImageFunctionFlag::HasCallSites))
        parts.callSites = static_cast<uint32_t>(readVarint(cursor));
    if (function.has(ImageFunctionFlag::HasHandlers)) {
        size_t size = static_cast<size_t>(readVarint(cursor));
        parts.handlers = unsafeMakeSpan(cursor, size);
        cursor += size;
    }
    if (function.has(ImageFunctionFlag::HasPositions))
        parts.positions = cursor;
    return parts;
}

inline void SideTableParts::append(Vector<uint8_t>& stream, const SideTableInputs& inputs)
{
    appendVarint(stream, inputs.frameRanges.size());
    stream.append(inputs.frameRanges);
    if (inputs.frameMapRange && inputs.frameMapRange->second) {
        appendVarint(stream, inputs.frameMapRange->first);
        appendVarint(stream, inputs.frameMapRange->second);
    }
    if (inputs.callSites)
        appendVarint(stream, *inputs.callSites);
    if (!inputs.handlers.empty()) {
        appendVarint(stream, inputs.handlers.size());
        stream.append(inputs.handlers);
    }
    stream.append(inputs.positions);
}

inline void SideTableParts::appendFrameRange(Vector<uint8_t>& bytes, uint32_t previousEnd, uint32_t begin, uint32_t end)
{
    RELEASE_ASSERT(previousEnd <= begin && begin <= end && !(begin % codeOffsetUnit) && !(end % codeOffsetUnit));
    appendVarint(bytes, (begin - previousEnd) / codeOffsetUnit);
    appendVarint(bytes, (end - begin) / codeOffsetUnit);
}

inline bool SideTableParts::hasFrameAt(uint32_t offset) const
{
    const uint8_t* cursor = frameRanges.data();
    const uint8_t* end = cursor + frameRanges.size();
    uint64_t position = offset / codeOffsetUnit;
    uint64_t boundary = 0;
    while (cursor < end) {
        boundary += readVarint(cursor);
        if (position < boundary)
            return false;
        boundary += readVarint(cursor);
        if (position < boundary)
            return true;
    }
    return false;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
