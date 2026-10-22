/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTImage.h"

#if ENABLE(AOT)

#include "AOTImageStamp.h"
#include "AOTRegExpRecord.h"
#include "AOTStubBlob.h"
#include "CPUFeatureSet.h"
#include "JSCellButterfly.h"
#include "MarkedBlockCage.h"
#include "Options.h"
#include <algorithm>
#include <bit>
#include <cstring>
#include <iterator>
#include <limits>
#include <wtf/StdLibExtras.h>
#include <wtf/TZoneMallocInlines.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

WTF_MAKE_TZONE_ALLOCATED_IMPL(Image);

std::atomic<const Image*> Image::s_current { nullptr };

static_assert(!offsetof(ImageHeader, magic) && offsetof(ImageHeader, stamp) == 8 && offsetof(ImageHeader, identity) == 16 && offsetof(ImageHeader, size) == 32, "check() reads magic, stamp and identity before it knows that the rest of the header has this build's layout");
static_assert(alignof(ImageHeader) <= imageSectionAlignment && alignof(ProgramTables) <= imageSectionAlignment);

static constexpr bool hasFixedInstructionSize = sizeOfNearCall == codeOffsetUnit;

static constexpr size_t elementSizeOf(ImageSection section)
{
    switch (section) {
    case ImageSection::Functions:
        return sizeof(ImageFunction);
    case ImageSection::InnerEntries:
        return sizeof(ImageInnerEntry);
    case ImageSection::Frames:
        return sizeof(ImageFrame);
    case ImageSection::ClosureWords:
        return sizeof(uint64_t);
    case ImageSection::Strings:
        return sizeof(ImageStringWord);
    case ImageSection::LongStrings:
        return sizeof(uint32_t);
    case ImageSection::Identifiers:
        return sizeof(ImageIdentifierWord);
    case ImageSection::StringHeaders:
#if ENABLE(AOT_POSITION_INDEPENDENT_IMAGE)
        return sizeof(ImageStringRecord);
#else
        return sizeof(ImageStringHeader);
#endif
    case ImageSection::RegExps:
        return sizeof(ImageRegExp);
    case ImageSection::RegExpHashes:
        return sizeof(ImageRegExpHash);
    case ImageSection::StubAddresses:
        return sizeof(StubAddressInImage);
    case ImageSection::StubReturnSites:
        return sizeof(StubReturnSite);
    case ImageSection::FunctionStarts:
    case ImageSection::CodeGranules:
    case ImageSection::FrameMaps:
    case ImageSection::SharedCodeCopies:
    case ImageSection::StringConstants:
    case ImageSection::SlotHandlers:
    case ImageSection::MethodLookups:
    case ImageSection::MethodTable:
        return sizeof(uint32_t);
    case ImageSection::Positions:
    case ImageSection::FaultSites:
    case ImageSection::SideTables:
    case ImageSection::Text:
    case ImageSection::RegExpDetails:
    case ImageSection::WebAssemblyDirectory:
    case ImageSection::ProgramTables:
        return 1;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

static ImageSectionRange rangeOf(const ImageHeader& header, ImageSection section)
{
    return header.sections[static_cast<unsigned>(section)];
}

static uint64_t numberOfElements(const ImageHeader& header, ImageSection section)
{
    return rangeOf(header, section).size / elementSizeOf(section);
}

static bool hasMatchingHeapOptions(const ImageHeader& header)
{
    return std::bit_cast<uint64_t>(header.heap.sizeClassProgression) == std::bit_cast<uint64_t>(Options::sizeClassProgressionWithAOT())
        && header.heap.markedBlockCageShift == MarkedBlockCage::shift;
}

static bool hasValidCodeRange(const ImageHeader& header)
{
    return !(header.size % imagePageSize)
        && !(header.codeOffset % imagePageSize)
        && header.codeOffset <= header.size
        && header.codeSize <= header.size - header.codeOffset
        && header.codeSize <= std::numeric_limits<uint32_t>::max();
}

static bool hasValidSectionRanges(const ImageHeader& header)
{
    uint64_t end = sizeof(ImageHeader);
    for (unsigned index = 0; index < numberOfImageSections; ++index) {
        ImageSectionRange range = header.sections[index];
        uint64_t rangeEnd = static_cast<uint64_t>(range.offset) + range.size;
        if (range.offset % imageSectionAlignment || range.offset < end || rangeEnd > header.codeOffset)
            return false;
        if (range.size % elementSizeOf(static_cast<ImageSection>(index)))
            return false;
        if (static_cast<ImageSection>(index) == ImageSection::MethodTable && range.size && range.offset % imagePageSize)
            return false;
        end = rangeEnd;
    }
    return true;
}

static bool hasValidFunctionTables(const ImageHeader& header)
{
    uint64_t numberOfFunctions = header.counts.functions;
    if (numberOfElements(header, ImageSection::Functions) != numberOfFunctions
        || numberOfElements(header, ImageSection::FunctionStarts) != numberOfFunctions + 1)
        return false;
    if (header.code.functionsEnd > header.codeSize || header.code.codeGranuleShift >= 32)
        return false;
    uint64_t granuleSize = 1ull << header.code.codeGranuleShift;
    uint64_t numberOfGranules = (header.code.functionsEnd + granuleSize - 1) >> header.code.codeGranuleShift;
    return numberOfElements(header, ImageSection::CodeGranules) >= numberOfGranules;
}

static bool hasValidSharedCodeCopies(const ImageHeader& header, std::span<const uint8_t> bytes)
{
    ImageSectionRange range = rangeOf(header, ImageSection::SharedCodeCopies);
    std::span copies { reinterpret_cast<const uint32_t*>(bytes.data() + range.offset), range.size / sizeof(uint32_t) };
    uint64_t endOfPreviousCopy = 0;
    for (uint32_t copy : copies) {
        if (copy < endOfPreviousCopy || copy > header.code.functionsEnd || header.code.sharedCodeSize > header.code.functionsEnd - copy)
            return false;
        endOfPreviousCopy = static_cast<uint64_t>(copy) + header.code.sharedCodeSize;
    }
    return true;
}

static uint64_t endOfCodePart(uint32_t offset, uint32_t size)
{
    return static_cast<uint64_t>(offset) + size;
}

static bool hasValidCodeParts(const ImageHeader& header)
{
    const ImageCodeRanges& code = header.code;
    return code.functionsEnd <= code.regExpCodeOffset
        && code.regExpCodeOffset <= code.webAssemblyCodeOffset
        && endOfCodePart(code.webAssemblyCodeOffset, code.webAssemblyCodeSize) <= code.constantsOffset
        && endOfCodePart(code.constantsOffset, code.constantsSize) <= code.ffiCodeOffset
        && endOfCodePart(code.ffiCodeOffset, code.ffiCodeSize) <= header.codeSize
        && (!code.ffiCodeSize || !(code.ffiCodeOffset % imagePageSize));
}

static const ProgramTables& programTablesOf(const ImageHeader& header, std::span<const uint8_t> bytes)
{
    return *reinterpret_cast<const ProgramTables*>(bytes.data() + rangeOf(header, ImageSection::ProgramTables).offset);
}

static bool hasProgramTables(const ImageHeader& header, std::span<const uint8_t> bytes)
{
    return rangeOf(header, ImageSection::ProgramTables).size >= sizeof(ProgramTables)
        && programTablesOf(header, bytes).magic == ProgramTables::expectedMagic;
}

static bool hasValidClosureWords(const ImageHeader& header, const ProgramTables& tables)
{
    const ClosureFunctionRangeData& range = tables.closureFunctions;
    return range.omittedWordCount <= range.first
        && range.first <= range.end
        && numberOfElements(header, ImageSection::ClosureWords) == range.end - range.omittedWordCount;
}

static bool hasValidText(const ImageHeader& header, const ProgramTables& tables)
{
    return numberOfElements(header, ImageSection::StringConstants) == tables.instanceSizes.numberOfStringCells
        && numberOfElements(header, ImageSection::StringHeaders) >= numberOfElements(header, ImageSection::Identifiers);
}

static bool hasWellFormedTables(const ImageHeader& header, std::span<const uint8_t> bytes)
{
    if (!hasValidCodeRange(header)
        || !hasValidSectionRanges(header)
        || !hasValidFunctionTables(header)
        || !hasValidSharedCodeCopies(header, bytes)
        || !hasValidCodeParts(header)
        || !hasProgramTables(header, bytes))
        return false;
    const ProgramTables& tables = programTablesOf(header, bytes);
    return hasValidClosureWords(header, tables) && hasValidText(header, tables);
}

Wire::ImageVerdict Image::check(std::span<const uint8_t> bytes, const std::array<uint8_t, 16>& identity)
{
    uint64_t magic;
    if (bytes.size() < sizeof(magic))
        return Wire::ImageVerdict::NotAnImage;
    memcpy(&magic, bytes.data(), sizeof(magic));
    if (magic != imageMagic)
        return Wire::ImageVerdict::NotAnImage;
    if (std::bit_cast<uintptr_t>(bytes.data()) % imageSectionAlignment)
        return Wire::ImageVerdict::Misaligned;
    if (bytes.size() < offsetof(ImageHeader, size))
        return Wire::ImageVerdict::Truncated;

    auto& header = *reinterpret_cast<const ImageHeader*>(bytes.data());
    if (header.identity != identity)
        return Wire::ImageVerdict::IdentityMismatch;
    if (header.stamp != imageStamp(CPUFeatureSet::buildTarget(), identity))
        return Wire::ImageVerdict::StampMismatch;
    if (bytes.size() < sizeof(ImageHeader) || header.size > bytes.size())
        return Wire::ImageVerdict::Truncated;
    if (!header.options.useWideReferences() && !MarkedBlockCage::isEnabled())
        return Wire::ImageVerdict::NeedsCellCage;
    if (!header.options.isCompatibleWithProcess() || !hasMatchingHeapOptions(header))
        return Wire::ImageVerdict::OptionsConflict;
    if (!hasWellFormedTables(header, bytes))
        return Wire::ImageVerdict::MalformedTables;
    if (!header.codeSize)
        return Wire::ImageVerdict::NoCode;
    return Wire::ImageVerdict::Accepted;
}

static const ImageHeader* findHeaderOfThisCompiler(std::span<const uint8_t> bytes)
{
    if (bytes.size() < sizeof(ImageHeader) || std::bit_cast<uintptr_t>(bytes.data()) % imageSectionAlignment)
        return nullptr;
    auto* header = reinterpret_cast<const ImageHeader*>(bytes.data());
    if (header->magic != imageMagic || header->stamp != imageStamp(CPUFeatureSet::buildTarget(), header->identity))
        return nullptr;
    return header;
}

std::optional<Wire::AdmittedCode> Image::admittedCode(std::span<const uint8_t> bytes)
{
    const ImageHeader* header = findHeaderOfThisCompiler(bytes);
    if (!header)
        return std::nullopt;
    return header->options.admitted();
}

std::optional<OptionSet<ImageFlag>> Image::flags(std::span<const uint8_t> bytes)
{
    const ImageHeader* header = findHeaderOfThisCompiler(bytes);
    if (!header)
        return std::nullopt;
    return OptionSet<ImageFlag>::fromRaw(header->flags);
}

Image::Image(std::span<const uint8_t> bytes, const uint8_t* code)
    : m_bytes(bytes)
    , m_code(code)
    , m_functions(section<ImageFunction>(ImageSection::Functions).data())
    , m_functionStarts(section<uint32_t>(ImageSection::FunctionStarts).data())
    , m_codeGranules(section<uint32_t>(ImageSection::CodeGranules).data())
    , m_text(*this)
{
    forgetRememberedPositions();
}

std::unique_ptr<Image> Image::create(std::span<const uint8_t> bytes, const uint8_t* code)
{
    return std::unique_ptr<Image>(new Image(bytes, code));
}

bool Image::publish(std::unique_ptr<Image>&& unpublishedImage)
{
    std::unique_ptr<Image> image = WTF::move(unpublishedImage);
    RELEASE_ASSERT(image);
    const Image* expected = nullptr;
    if (!s_current.compare_exchange_strong(expected, image.get(), std::memory_order_release, std::memory_order_relaxed))
        return false;
    JSCellButterfly::setStorageOutsideHeap(image->m_bytes);
    image.release();
    return true;
}

std::span<const uint8_t> Image::regExpCode() const
{
    const ImageCodeRanges& ranges = header().code;
    return code().subspan(ranges.regExpCodeOffset, ranges.webAssemblyCodeOffset - ranges.regExpCodeOffset);
}

std::span<const uint8_t> Image::webAssemblyCode() const
{
    const ImageCodeRanges& ranges = header().code;
    return code().subspan(ranges.webAssemblyCodeOffset, ranges.webAssemblyCodeSize);
}

std::span<const uint8_t> Image::ffiCode() const
{
    const ImageCodeRanges& ranges = header().code;
    return code().subspan(ranges.ffiCodeOffset, ranges.ffiCodeSize);
}

std::span<const StubReturnSite> Image::stubReturnSites() const
{
    return section<StubReturnSite>(ImageSection::StubReturnSites);
}

static CodeLocation locateAfterFunctions(const ImageCodeRanges& ranges, uint32_t offset)
{
    if (offset - ranges.webAssemblyCodeOffset < ranges.webAssemblyCodeSize)
        return { CodeKind::WebAssembly, StubFrameKind::Plain, 0, offset - ranges.webAssemblyCodeOffset };
    if (offset - ranges.ffiCodeOffset < ranges.ffiCodeSize)
        return { CodeKind::FFI, StubFrameKind::Plain, 0, offset - ranges.ffiCodeOffset };
    if (offset >= ranges.regExpCodeOffset && offset < ranges.webAssemblyCodeOffset)
        return { CodeKind::RegExp, StubFrameKind::Plain, 0, offset - ranges.regExpCodeOffset };
    return { };
}

CodeLocation Image::locate(const void* pc) const
{
    auto& header = this->header();
    uintptr_t distance = std::bit_cast<uintptr_t>(pc) - std::bit_cast<uintptr_t>(m_code);
    if (distance >= header.codeSize)
        return { };
    uint32_t offset = static_cast<uint32_t>(distance);
    if (offset >= header.code.functionsEnd)
        return locateAfterFunctions(header.code, offset);
    std::span copies = sharedCodeCopies();
    auto nextCopy = std::ranges::upper_bound(copies, offset);
    if (nextCopy != copies.begin()) {
        uint32_t inSharedCode = offset - *std::prev(nextCopy);
        if (inSharedCode < header.code.sharedCodeSize) {
            std::span sites = stubReturnSites();
            auto site = std::ranges::lower_bound(sites, inSharedCode, { }, &StubReturnSite::offset);
            bool isReturnSite = site != sites.end() && site->offset == inSharedCode;
            return { CodeKind::Stub, isReturnSite ? site->kind : StubFrameKind::Plain, 0, inSharedCode };
        }
    }
    const uint32_t* starts = m_functionStarts;
    uint32_t index = m_codeGranules[offset >> header.code.codeGranuleShift];
    if (offset < starts[index])
        return { };
    while (starts[index + 1] <= offset)
        ++index;
    return { CodeKind::Function, StubFrameKind::Plain, index, offset - starts[index] };
}

const void* Image::stubAddress(StubKey key) const
{
    if (sharedCodeCopies().empty())
        return nullptr;
    std::span addresses = section<StubAddressInImage>(ImageSection::StubAddresses);
    auto address = std::ranges::lower_bound(addresses, key.bits(), { }, &StubAddressInImage::key);
    if (address == addresses.end() || address->key != key.bits())
        return nullptr;
    return sharedCodeCopy(0) + address->offset;
}

// The family is the most significant part of a key, so the stubs of a family lie side by side, by form and then by count.
// Whoever wants all of them searches once.
std::span<const StubAddressInImage> Image::stubAddressesOf(StubFamily family) const
{
    auto addresses = std::ranges::equal_range(section<StubAddressInImage>(ImageSection::StubAddresses), family, { }, [](const StubAddressInImage& address) {
        return StubKey::fromBits(address.key).family();
    });
    return { addresses.begin(), addresses.end() };
}

bool Image::hasFrameAt(uint32_t function, uint32_t offset) const
{
    return SideTableParts::read(this->function(function), section<uint8_t>(ImageSection::SideTables).data()).hasFrameAt(offset);
}

bool Image::hasFrameAtReturnOffset(uint32_t function, uint32_t returnOffset) const
{
    return hasFrameAtReturnOffset(function, SideTableParts::read(this->function(function), section<uint8_t>(ImageSection::SideTables).data()), returnOffset);
}

bool Image::hasFrameAtReturnOffset(uint32_t function, const SideTableParts& parts, uint32_t returnOffset) const
{
    if (!returnOffset)
        return false;
    if constexpr (!hasFixedInstructionSize) {
        // The fault handler makes the address of a faulting instruction plus sizeOfNearCall the return address, whatever the length of the instruction.
        // No instruction that faults begins as a near call does, and most return addresses stand behind one: they are not looked for in the fault table.
        constexpr uint8_t opcodeOfNearCall = 0xe8;
        if (returnOffset >= sizeOfNearCall && codeOf(function)[returnOffset - sizeOfNearCall] != opcodeOfNearCall) {
            std::optional<FaultSite> fault = faultSiteAt(codeOf(function) + (returnOffset - sizeOfNearCall));
            if (fault && fault->kind == FaultSiteKind::NullCheck)
                return fault->hasFrame;
        }
    }
    return parts.hasFrameAt(returnOffset - 1);
}

static std::span<const uint8_t> bytesOf(const StringImpl& string) { return string.is8Bit() ? asBytes(string.span8()) : asBytes(string.span16()); }

bool Image::containsCharactersOf(const StringImpl& string) const
{
    uintptr_t characters = std::bit_cast<uintptr_t>(bytesOf(string).data());
    return characters - std::bit_cast<uintptr_t>(m_bytes.data()) < m_bytes.size();
}

bool isCompiledCodePC(const void* pc)
{
    return !!imageCodeOffset(pc);
}

std::optional<uint32_t> imageCodeOffset(const void* address)
{
    const Image* image = Image::current();
    if (!image)
        return std::nullopt;
    uintptr_t offset = std::bit_cast<uintptr_t>(address) - std::bit_cast<uintptr_t>(image->code().data());
    if (offset >= image->header().codeSize)
        return std::nullopt;
    return static_cast<uint32_t>(offset);
}

std::optional<ImageFrame> imageFrameAtReturnAddress(const void* returnAddress)
{
    const Image* image = Image::current();
    if (!image)
        return std::nullopt;
    CodeLocation code = image->locate(returnAddress);
    switch (code.kind) {
    case CodeKind::NotInImage:
        return std::nullopt;
    case CodeKind::RegExp:
    case CodeKind::WebAssembly:
    case CodeKind::FFI:
        return ImageFrame { };
    case CodeKind::Function:
        return image->frameAtReturnOffset(code.function, code.offset);
    case CodeKind::Stub: {
        ImageFrame frame = code.stubFrameKind == StubFrameKind::AtStubEntry ? ImageFrame::empty() : ImageFrame { };
        if (bytesAboveReturnAddressOf(code.stubFrameKind))
            frame.calleeSavesStart |= ImageFrame::liesBelowSpilledArgumentsBit;
        return frame;
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

uintptr_t imageCodeStart()
{
    const Image* image = Image::current();
    return image ? std::bit_cast<uintptr_t>(image->code().data()) : 0;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
