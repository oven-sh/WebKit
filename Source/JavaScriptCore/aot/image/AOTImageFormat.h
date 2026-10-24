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
#include "AOTVector.h"
#include "CPU.h"
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
// Goes up whenever a member of ImageHeader in front of `options` changes its place or its meaning. 1 had no such member, and all of its code part was executable.
static constexpr uint32_t imageFormatVersion = 2;
static constexpr size_t imagePageSize = std::max<size_t>(CeilingOnPageSize, 16 * KB);
static constexpr size_t imageFunctionAlignment = isX86() ? 2 : sizeof(uint32_t);
static constexpr size_t imageStubsAlignment = 16;
static constexpr size_t imageSectionAlignment = 16;
static constexpr size_t imageConstantsAlignment = 16;
static constexpr size_t imageFFIDirectoryAlignment = 4;
constexpr uint32_t alignmentOfConstant(size_t size) { return static_cast<uint32_t>(std::min(imageConstantsAlignment, size_t { 1 } << std::countr_zero(size))); }
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
    InnerEntries,
    Frames,
    FrameMaps,
    ClosureWords,
    Positions,
    FaultSites,
    SideTables,
    SharedCodeCopies,
    Strings,
    LongStrings,
    Text,
    Identifiers,
    StringConstants,
    StringHeaders,
    RegExps,
    RegExpHashes,
    RegExpDetails,
    WebAssemblyDirectory,
    ProgramTables,
    StubAddresses,
    StubReturnSites,
    SlotHandlers,
    // The code offset of the lookup of each selector of the method table, in units of codeOffsetUnit.
    MethodLookups,
    // The code offsets that calls through the method table load. Each instance maps a copy-on-write clone of it, so it begins at a page and nothing else lies in its pages. Nothing writes it.
    MethodTable,
};
static constexpr unsigned numberOfImageSections = 27;
static_assert(static_cast<unsigned>(ImageSection::MethodTable) + 1 == numberOfImageSections);

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

// In the order in which the parts lie. The section of the regular expressions ends at constantsOffset, and ImageHeader::executableSize lies in it: in front of its tables. Where there is FFI code, its directory lies behind the constants and ends the code part.
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

// Without AdmitsUnknownJavaScript the proofs in the code hold only while nothing but the code of the image runs, and without LoadsModulesByName only while no module is found by a name.
// Without LoadsCodeOutsideImage they hold only while no file with code is loaded but those that the build lists as staying outside.
enum class ImageFlag : uint32_t {
    IsObfuscated = 1 << 0,
    AdmitsUnknownJavaScript = 1 << 1,
    LoadsModulesByName = 1 << 2,
    AdmitsLibraryText = 1 << 3,
    ShowsWaysBetweenModuleObjects = 1 << 4,
    LoadsCodeOutsideImage = 1 << 5,
};

// codeOffset and sections[].offset count from the header.
struct ImageHeader {
    bool has(ImageFlag flag) const { return flags & static_cast<uint32_t>(flag); }
    bool isObfuscated() const { return has(ImageFlag::IsObfuscated); }
    bool admitsUnknownJavaScript() const { return has(ImageFlag::AdmitsUnknownJavaScript); }
    bool loadsModulesByName() const { return has(ImageFlag::LoadsModulesByName); }
    bool admitsLibraryText() const { return has(ImageFlag::AdmitsLibraryText); }
    bool showsWaysBetweenModuleObjects() const { return has(ImageFlag::ShowsWaysBetweenModuleObjects); }
    bool loadsCodeOutsideImage() const { return !runsOnlyCodeOfImage && has(ImageFlag::LoadsCodeOutsideImage); }

    uint64_t magic;
    uint64_t stamp;
    std::array<uint8_t, 16> identity;
    uint64_t size;
    uint64_t codeOffset;
    uint64_t codeSize;
    uint64_t address;
    // The code part begins with what is executed, on this many bytes, whole pages. The rest is data that the code finds by its distance: it is mapped with the code and is only read.
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
};
static_assert(std::is_trivially_copyable_v<ImageHeader> && std::is_standard_layout_v<ImageHeader>);
static_assert(!(offsetof(ImageHeader, sections) % 4));
static_assert(!offsetof(ImageHeader, magic) && offsetof(ImageHeader, stamp) == 8 && offsetof(ImageHeader, identity) == 16);
static_assert(offsetof(ImageHeader, size) == 32 && offsetof(ImageHeader, codeOffset) == 40 && offsetof(ImageHeader, codeSize) == 48, "Bun's test/aot/cli/compiled-image.ts and test/aot/shell/expectations.py read these members by offset");
static_assert(offsetof(ImageHeader, address) == 56, "Bun's writers of executables list this member for the loader");
static_assert(offsetof(ImageHeader, executableSize) == 64, "Bun's writer of ELF executables reads this member by offset");
static_assert(offsetof(ImageHeader, formatVersion) == 72 && offsetof(ImageHeader, options) == 80, "whoever reads an image without this header finds the members in front of `options` by offset: they stay where they are");
static_assert(offsetof(ImageHeader, sections) == 208 && numberOfImageSections == 27 && offsetof(ImageHeader, code) + offsetof(ImageCodeRanges, constantsOffset) == 464 && offsetof(ImageHeader, code) + offsetof(ImageCodeRanges, constantsSize) == 468, "Bun's test/aot/cli/compiled-image.ts reads these members by offset");

enum class ImageFunctionFlag : uint8_t {
    HasInlineFrames = 1 << 0,
    HasHandlers = 1 << 1,
    HasFrameMaps = 1 << 2,
    HasCallSites = 1 << 3,
    HasPositions = 1 << 4,
    // At its entry, in front of the stack words of its caller and above its return address.
    SpillsArgumentRegisters = 1 << 5,
};

// function() is a FunctionID index. An image function index is a position in ImageSection::Functions.
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
static_assert(defaultLimitValue(LimitID::ImageFunctions, hostArchitecture()) == ClosureWord::firstReservedFunction);

// A frame is named by the place of its frame record: the word below its return address.
struct ImageFrame {
    static constexpr unsigned maxCalleeSavesStart = (1u << 6) - 1;
    static constexpr uint16_t hasNoFrameRecordBit = 1u << 15;
    // Not in the table: Image::frameAtReturnOffset() sets it from ImageFunctionFlag::SpillsArgumentRegisters, which holds where the function has no frame too.
    static constexpr uint16_t liesBelowSpilledArgumentsBit = 1u << 14;
    // Not in the table either: of code that has no frame record and nothing else on the stack, so that its return address is on top.
    static constexpr uint16_t isEmptyBit = 1u << 13;
    static constexpr ImageFrame empty() { return { 0, hasNoFrameRecordBit | isEmptyBit, 0 }; }
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
    bool hasFrameRecord() const { return !(calleeSavesStart & hasNoFrameRecordBit); }
    // In words below the place of the frame record.
    unsigned startOfCalleeSaves() const { return calleeSavesStart & maxCalleeSavesStart; }
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
    uint32_t reserved;
};
static_assert(sizeof(StubAddressInImage) == 16);

// SlotHandlers has an offset into the shared code for each member of PropertyCacheWords::handlers, in the order of that array.
static constexpr uint32_t offsetOfAbsentStub = 0xffffffffu;

// frameRanges says whether the function has its frame at a return address, and at nothing else: it holds the distances between the offsets at which the answer changes, in units of codeOffsetUnit.
struct SideTableInputs {
    std::span<const uint8_t> frameRanges;
    uint32_t frame { 0 };
    std::optional<std::pair<uint32_t, uint32_t>> frameMapRange;
    std::span<const uint8_t> handlers;
    std::span<const uint8_t> callSites;
};

struct SideTableParts {
    static SideTableParts read(const ImageFunction&, const uint8_t* sideTables);
    static void append(Vector<uint8_t>& stream, const SideTableInputs&);
    static void appendFrameRange(Vector<uint8_t>& bytes, uint32_t previousEnd, uint32_t begin, uint32_t end);
    static void appendFrameChange(Vector<uint8_t>& bytes, uint32_t previousChange, uint32_t change);

    bool hasFrameAt(uint32_t offset) const;

    std::span<const uint8_t> frameRanges;
    uint32_t frame { 0 };
    uint32_t firstFrameMapWord { 0 };
    uint32_t numberOfFrameMapWords { 0 };
    std::span<const uint8_t> handlers;
    const uint8_t* callSites { nullptr };
};

// ImageSection::Positions: for each function of the image, in their order, the size of its positions and the positions. In front of them the place of every 32nd of these entries.
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
    if (function.has(ImageFunctionFlag::HasCallSites))
        parts.callSites = cursor;
    return parts;
}

inline void SideTableParts::append(Vector<uint8_t>& stream, const SideTableInputs& inputs)
{
    appendVarint(stream, inputs.frameRanges.size());
    stream.append(inputs.frameRanges);
    if (!inputs.frameRanges.empty())
        appendVarint(stream, inputs.frame);
    if (inputs.frameMapRange && inputs.frameMapRange->second) {
        appendVarint(stream, inputs.frameMapRange->first);
        appendVarint(stream, inputs.frameMapRange->second);
    }
    if (!inputs.handlers.empty()) {
        appendVarint(stream, inputs.handlers.size());
        stream.append(inputs.handlers);
    }
    stream.append(inputs.callSites);
}

inline void SideTableParts::appendFrameRange(Vector<uint8_t>& bytes, uint32_t previousEnd, uint32_t begin, uint32_t end)
{
    RELEASE_ASSERT(previousEnd <= begin && begin <= end && !(begin % codeOffsetUnit) && !(end % codeOffsetUnit));
    appendVarint(bytes, (begin - previousEnd) / codeOffsetUnit);
    appendVarint(bytes, (end - begin) / codeOffsetUnit);
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
