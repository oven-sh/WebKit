/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTImage.h"

#if ENABLE(AOT)

#include "AOTRegExpRecord.h"
#include "AOTStubBlob.h"
#include "JSCellButterfly.h"
#include "MarkedBlockCage.h"
#include "Options.h"
#include <algorithm>
#include <cstring>
#include <iterator>
#include <limits>
#include <wtf/Lock.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/StdLibExtras.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/Threading.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

WTF_MAKE_TZONE_ALLOCATED_IMPL(Image);

std::atomic<const Image*> Image::s_current { nullptr };
std::atomic<const Image*> Image::s_lastPublished { nullptr };

static thread_local const Image* t_currentImage { nullptr };
static thread_local bool t_usesNoImage { false };
// Of a process with one image: a thread that has named none runs that one, or helps a thread that does.
static std::atomic<const Image*> s_onlyImage { nullptr };
static Lock s_publishedImagesLock;
static Vector<const Image*>& publishedImages() WTF_REQUIRES_LOCK(s_publishedImagesLock)
{
    static NeverDestroyed<Vector<const Image*>> images;
    return images.get();
}

const Image* Image::currentOfThread()
{
    if (t_currentImage || t_usesNoImage)
        return t_currentImage;
    return s_onlyImage.load(std::memory_order_acquire);
}

void Image::setCurrentOfThread(const Image* image)
{
    t_currentImage = image;
}

void Image::useNoneOnThisThread()
{
    t_currentImage = nullptr;
    t_usesNoImage = true;
    s_current.store(differsByThread(), std::memory_order_release);
}

const Image* Image::find(std::span<const uint8_t> bytes)
{
    Locker locker { s_publishedImagesLock };
    for (const Image* image : publishedImages()) {
        if (image->m_bytes.data() == bytes.data())
            return image;
    }
    return nullptr;
}

const Image* Image::findByCodeAddress(const void* address)
{
    for (const Image* image = s_lastPublished.load(std::memory_order_acquire); image; image = image->m_publishedBefore) {
        if (std::bit_cast<uintptr_t>(address) - std::bit_cast<uintptr_t>(image->m_code) < image->header().codeSize)
            return image;
    }
    return nullptr;
}

void Image::forEachPublishedAsCurrent(const ScopedLambda<IterationStatus(const Image&)>& functor)
{
    const Image* currentBefore = t_currentImage;
    for (const Image* image = s_lastPublished.load(std::memory_order_acquire); image; image = image->m_publishedBefore) {
        t_currentImage = image;
        if (functor(*image) == IterationStatus::Done)
            break;
    }
    t_currentImage = currentBefore;
}

static_assert(!offsetof(ImageHeader, magic) && offsetof(ImageHeader, stamp) == 8 && offsetof(ImageHeader, identity) == 16 && offsetof(ImageHeader, size) == 32, "check() reads magic, stamp and identity before it knows that the rest of the header has this build's layout");
static_assert(alignof(ImageHeader) <= imageSectionAlignment && alignof(ProgramTables) <= imageSectionAlignment);

static constexpr bool hasFixedInstructionSize = sizeOfNearCall == codeOffsetUnit;

#if ENABLE(AOT_POSITION_INDEPENDENT_IMAGE)
using ImageStringHeaderOrRecord = ImageStringRecord;
#else
using ImageStringHeaderOrRecord = ImageStringHeader;
#endif

static constexpr size_t elementSizes[] = {
#define AOT_SIZE_OF_ELEMENT(name, Element, statistic) sizeof(Element),
    FOR_EACH_AOT_IMAGE_SECTION(AOT_SIZE_OF_ELEMENT)
#undef AOT_SIZE_OF_ELEMENT
};
static_assert(std::size(elementSizes) == numberOfImageSections);

static bool hasMatchingHeapOptions(const ImageHeader& header)
{
    return std::bit_cast<uint64_t>(header.heap.sizeClassProgression) == std::bit_cast<uint64_t>(Options::sizeClassProgressionWithAOT())
        && header.heap.markedBlockCageShift == MarkedBlockCage::shift;
}

static uint64_t endOfCodePart(uint32_t offset, uint32_t size)
{
    return static_cast<uint64_t>(offset) + size;
}

static bool hasWellFormedTables(const ImageHeader& header, std::span<const uint8_t> bytes)
{
    const ImageCodeRanges& code = header.code;
    auto rangeOf = [&](ImageSection section) {
        return header.sections[static_cast<unsigned>(section)];
    };
    auto numberOfElements = [&](ImageSection section) -> uint64_t {
        return rangeOf(section).size / elementSizes[static_cast<unsigned>(section)];
    };

    bool hasValidCodeRange = !(header.size % imagePageSize)
        && !(header.codeOffset % imagePageSize)
        && header.codeOffset <= header.size
        && header.codeSize <= header.size - header.codeOffset
        && header.codeSize <= std::numeric_limits<uint32_t>::max()
        && !(header.executableSize % imagePageSize)
        && header.executableSize <= header.codeSize;
    if (!hasValidCodeRange)
        return false;

    uint64_t end = sizeof(ImageHeader);
    for (unsigned index = 0; index < numberOfImageSections; ++index) {
        ImageSectionRange range = header.sections[index];
        uint64_t rangeEnd = static_cast<uint64_t>(range.offset) + range.size;
        if (range.offset % imageSectionAlignment || range.offset < end || rangeEnd > header.codeOffset)
            return false;
        if (range.size % elementSizes[index])
            return false;
        if (static_cast<ImageSection>(index) == ImageSection::MethodTable && range.size && range.offset % imagePageSize)
            return false;
        end = rangeEnd;
    }

    uint64_t numberOfFunctions = header.counts.functions;
    if (numberOfElements(ImageSection::Functions) != numberOfFunctions
        || numberOfElements(ImageSection::FunctionStarts) != numberOfFunctions + 1)
        return false;
    if (code.functionsEnd > header.codeSize || code.codeGranuleShift >= 32)
        return false;
    uint64_t granuleSize = 1ull << code.codeGranuleShift;
    if (numberOfElements(ImageSection::CodeGranules) < (code.functionsEnd + granuleSize - 1) >> code.codeGranuleShift)
        return false;

    std::span copies { reinterpret_cast<const uint32_t*>(bytes.data() + rangeOf(ImageSection::SharedCodeCopies).offset), static_cast<size_t>(numberOfElements(ImageSection::SharedCodeCopies)) };
    uint64_t endOfPreviousCopy = 0;
    for (uint32_t copy : copies) {
        if (copy < endOfPreviousCopy || copy > code.functionsEnd || code.sharedCodeSize > code.functionsEnd - copy)
            return false;
        endOfPreviousCopy = static_cast<uint64_t>(copy) + code.sharedCodeSize;
    }

    bool hasValidCodeParts = code.functionsEnd <= code.webAssemblyCodeOffset
        && endOfCodePart(code.webAssemblyCodeOffset, code.webAssemblyCodeSize) <= code.ffiCodeOffset
        && endOfCodePart(code.ffiCodeOffset, code.ffiCodeSize) <= code.regExpCodeOffset
        && code.regExpCodeOffset <= header.executableSize
        && header.executableSize <= code.constantsOffset
        && endOfCodePart(code.constantsOffset, code.constantsSize) <= header.codeSize
        && (!code.ffiCodeSize || !(code.ffiCodeOffset % imagePageSize));
    if (!hasValidCodeParts || rangeOf(ImageSection::ProgramTables).size < sizeof(ProgramTables))
        return false;

    auto& tables = *reinterpret_cast<const ProgramTables*>(bytes.data() + rangeOf(ImageSection::ProgramTables).offset);
    const ClosureFunctionRangeData& closures = tables.closureFunctions;
    return tables.magic == ProgramTables::expectedMagic
        && closures.omittedWordCount <= closures.first
        && closures.first <= closures.end
        && numberOfElements(ImageSection::ClosureWords) == closures.end - closures.omittedWordCount
        && numberOfElements(ImageSection::StringConstants) == tables.instanceSizes.numberOfStringCells
        && numberOfElements(ImageSection::StringHeaders) >= numberOfElements(ImageSection::Identifiers);
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
    if (header.formatVersion != imageFormatVersion)
        return Wire::ImageVerdict::StampMismatch;
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

void Image::publish(std::unique_ptr<Image>&& unpublishedImage, std::span<const uint8_t> allImages)
{
    std::unique_ptr<Image> image = WTF::move(unpublishedImage);
    RELEASE_ASSERT(image);
    Locker locker { s_publishedImagesLock };
    Vector<const Image*>& images = publishedImages();
    image->m_number = images.size();
    if (images.isEmpty()) {
        JSCellButterfly::setStorageOutsideHeap(allImages.empty() ? image->m_bytes : allImages);
        Thread::setSurvivesFailedCreation();
    }
    RELEASE_ASSERT(JSCellButterfly::isOutsideHeap(std::bit_cast<const Butterfly*>(image->m_bytes.data())) && JSCellButterfly::isOutsideHeap(std::bit_cast<const Butterfly*>(image->m_bytes.data() + image->m_bytes.size() - 1)));
    t_currentImage = image.get();
    image->m_publishedBefore = s_lastPublished.load(std::memory_order_relaxed);
    s_lastPublished.store(image.get(), std::memory_order_release);
    s_onlyImage.store(images.isEmpty() ? image.get() : nullptr, std::memory_order_release);
    s_current.store(images.isEmpty() ? image.get() : differsByThread(), std::memory_order_release);
    images.append(image.release());
}

std::span<const uint8_t> Image::ffiDirectory() const
{
    const ImageCodeRanges& ranges = header().code;
    uint64_t offset = WTF::roundUpToMultipleOf<imageFFIDirectoryAlignment>(endOfCodePart(ranges.constantsOffset, ranges.constantsSize));
    if (offset > header().codeSize)
        return { };
    return code().subspan(offset);
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
    if (offset >= ranges.regExpCodeOffset && offset < ranges.constantsOffset)
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
std::span<const StubAddressInImage> Image::stubAddressesOf(StubFamily family) const
{
    auto addresses = std::ranges::equal_range(section<StubAddressInImage>(ImageSection::StubAddresses), family, { }, [](const StubAddressInImage& address) {
        return StubKey::fromBits(address.key).family();
    });
    return { addresses.begin(), addresses.end() };
}

bool Image::usesCallConvention(uint32_t offsetInSharedCode) const
{
    return std::ranges::any_of(section<StubAddressInImage>(ImageSection::StubAddresses), [&](const StubAddressInImage& address) {
        return address.offset == offsetInSharedCode && address.usesCallConvention;
    });
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
