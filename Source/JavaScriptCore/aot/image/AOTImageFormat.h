/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTClosureWord.h"
#include "AOTCompileOptions.h"
#include "AOTLimits.h"
#include "AOTPrefixCode.h"
#include "AOTRecordTypes.h"
#include "AOTRuntimeTable.h"
#include "AOTStubFrames.h"
#include "AOTVarint.h"
#include "AOTVector.h"
#include "CPU.h"
#include "CPUFeatureSet.h"
#include "JSExportMacros.h"
#include "StackAlignment.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <limits>
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
static constexpr uint32_t imageFormatVersion = 5;
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

class FunctionStarts {
public:
    static constexpr unsigned groupShift = 5;
    static constexpr uint32_t functionsInGroup = 1u << groupShift;
    static constexpr uint32_t wideGroup = 1u << 31;

    static constexpr size_t numberOfWordsOfSteps(size_t numberOfFunctions) { return (numberOfFunctions + 2) / 2; }
    static constexpr size_t numberOfGroupsFor(size_t numberOfFunctions) { return (numberOfFunctions + functionsInGroup) / functionsInGroup; }
    static constexpr size_t numberOfWordsWithoutLists(size_t numberOfFunctions) { return numberOfWordsOfSteps(numberOfFunctions) + numberOfGroupsFor(numberOfFunctions); }
    JS_EXPORT_PRIVATE static Vector<uint8_t> encode(std::span<const uint32_t> starts);

    FunctionStarts(const uint32_t* words, size_t numberOfFunctions)
        : m_steps(reinterpret_cast<const uint16_t*>(words))
        , m_groups(words + numberOfWordsOfSteps(numberOfFunctions))
    {
    }

    uint32_t operator[](uint32_t function) const
    {
        uint32_t group = m_groups[function >> groupShift];
        if (group & wideGroup) [[unlikely]]
            return m_groups[(group & ~wideGroup) + function % functionsInGroup];
        return group + m_steps[function];
    }

    uint32_t startOfGroup(uint32_t group) const
    {
        uint32_t start = m_groups[group];
        return start & wideGroup ? m_groups[start & ~wideGroup] : start;
    }

    uint32_t find(uint32_t offset, uint32_t firstGroup, uint32_t lastGroup) const
    {
        uint32_t group = firstGroup;
        while (group < lastGroup && startOfGroup(group + 1) <= offset)
            ++group;
        uint32_t firstFunction = group << groupShift;
        uint32_t start = m_groups[group];
        if (start & wideGroup) [[unlikely]]
            return firstFunction + indexOfLastUpTo(m_groups + (start & ~wideGroup), offset);
        return firstFunction + indexOfLastUpTo(m_steps + firstFunction, std::min<uint32_t>(offset - start, std::numeric_limits<uint16_t>::max()));
    }

    const uint16_t* steps() const { return m_steps; }
    const uint32_t* groups() const { return m_groups; }

private:
    template<typename T>
    static uint32_t indexOfLastUpTo(const T* first, uint32_t value)
    {
        uint32_t index = 0;
        for (uint32_t step = functionsInGroup / 2; step; step /= 2)
            index += first[index + step] <= value ? step : 0;
        return index;
    }

    const uint16_t* m_steps;
    const uint32_t* m_groups;
};

#define FOR_EACH_AOT_IMAGE_SECTION(macro) \
    macro(Functions, uint32_t) \
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
    macro(Identifiers, uint8_t) \
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
    HasReloadableModules = 1 << 11,
    RefusesOriginalsBesideMocks = 1 << 12,
};

struct ImageNumberRanges {
    uint32_t lastShape;
    uint32_t endOfClosureNumbers;
    uint32_t endOfIdentifiers;
    uint32_t endOfStringHeaders;
    uint32_t numberOfStringWords;
    uint32_t numberOfCodedTexts;
    uint32_t numberOfPlacedConstants;
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
    OptionSet<ImageFunctionFlag> flags() const { return OptionSet<ImageFunctionFlag>::fromRaw(flagBits); }
    bool has(ImageFunctionFlag flag) const { return flags().contains(flag); }

    uint32_t sideTables;
    uint8_t flagBits;
};

class ImageFunctions {
public:
    static constexpr unsigned functionsInGroup = 32;
    static constexpr size_t wordsOfGroup = 3;
    static constexpr uint16_t isListedPlace = 1u << 15;
    static constexpr uint32_t hasWholePlaces = UINT32_MAX;

    ImageFunctions(const uint32_t* words, uint32_t count)
        : m_places(words + wordsOfHead)
        , m_groups(words + firstGroupWord(words[0], count))
        , m_listedPlaces(words + firstListWord(words[0], count))
        , m_listedNumbers(reinterpret_cast<const Number24*>(m_listedPlaces + numberOfListedPlaces(words[0])))
        , m_hasWholePlaces(words[0] == hasWholePlaces)
    {
    }

    uint32_t sideTableOf(uint32_t index) const
    {
        if (m_hasWholePlaces) [[unlikely]]
            return m_places[index];
        uint16_t step = reinterpret_cast<const uint16_t*>(m_places)[index];
        return step & isListedPlace ? m_listedPlaces[step & ~isListedPlace] : group(index)[2] + step;
    }
    uint32_t numberOf(uint32_t index) const
    {
        uint32_t entry = index % functionsInGroup;
        uint32_t listed = ~group(index)[0] & UINT32_MAX >> (functionsInGroup - 1 - entry);
        uint32_t lastListed = functionsInGroup - 1 - std::countl_zero(listed);
        return m_listedNumbers[group(index)[1] + std::popcount(listed) - 1] + (entry - lastListed);
    }

    static bool hasSize(std::span<const uint32_t> words, size_t count) { return words.size() >= wordsOfHead && words[2] == count && words.size() == firstListWord(words[0], count) + numberOfListedPlaces(words[0]) + wordsOfNumbers(words[1]); }
    JS_EXPORT_PRIVATE static Vector<uint8_t> encode(std::span<const uint32_t> sideTables, std::span<const uint32_t> numbers);

private:
    static constexpr size_t wordsOfHead = 3;

    static constexpr size_t numberOfListedPlaces(uint32_t firstWord) { return firstWord == hasWholePlaces ? 0 : firstWord; }
    static constexpr size_t firstGroupWord(uint32_t firstWord, size_t count) { return wordsOfHead + (firstWord == hasWholePlaces ? count : (count + 1) / 2); }
    static constexpr size_t firstListWord(uint32_t firstWord, size_t count) { return firstGroupWord(firstWord, count) + (count + functionsInGroup - 1) / functionsInGroup * wordsOfGroup; }
    static constexpr size_t wordsOfNumbers(size_t count) { return (count * sizeof(Number24) + sizeof(uint32_t) - 1) / sizeof(uint32_t); }

    const uint32_t* group(uint32_t index) const { return m_groups + index / functionsInGroup * wordsOfGroup; }

    const uint32_t* m_places;
    const uint32_t* m_groups;
    const uint32_t* m_listedPlaces;
    const Number24* m_listedNumbers;
    bool m_hasWholePlaces;
};
static_assert(defaultLimitValue(LimitID::ImageFunctions, hostArchitecture()) <= static_cast<uint64_t>(Number24::max) + 1);
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

struct PositionIndex {
    static constexpr unsigned functionsBetween = 16;
    static constexpr unsigned numberOfCodes = 21;
    static constexpr size_t numberOfEntriesFor(size_t numberOfFunctions) { return (numberOfFunctions + functionsBetween - 1) / functionsBetween; }

    static const uint8_t* firstEntry(const uint8_t* section, size_t group, size_t numberOfFunctions)
    {
        const uint8_t* index = PrefixCodes<numberOfCodes>::endOfCodes(section);
        uint32_t place;
        memcpy(&place, index + group * sizeof(uint32_t), sizeof(uint32_t));
        return index + numberOfEntriesFor(numberOfFunctions) * sizeof(uint32_t) + place;
    }
};

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
