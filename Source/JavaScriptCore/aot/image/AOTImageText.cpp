/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTImageText.h"

#if ENABLE(AOT)

#include "AOTCodedTexts.h"
#include "AOTImage.h"
#include "AOTState.h"
#include "AOTVector.h"
#include <algorithm>
#include <array>
#include <bit>
#include <wtf/HashMap.h>
#include <wtf/Lock.h>
#include <wtf/MathExtras.h>
#include <wtf/OSAllocator.h>
#include <wtf/PageBlock.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Threading.h>
#include <wtf/text/AtomStringImpl.h>
#include <wtf/text/StringHasherInlines.h>
#include <wtf/text/StringImpl.h>

#if !ENABLE(AOT_POSITION_INDEPENDENT_IMAGE)
#if OS(WINDOWS)
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#endif

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

static_assert(ImageString::hashMask == StringHasher::maskHash && ImageIdentifier::hashMask == StringHasher::maskHash);

static_assert(!(imageSectionAlignment % alignof(ImageStringRecord)) && !(imageSectionAlignment % alignof(ImageStringHeader)));

struct ImageText::DecodedTexts {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(DecodedTexts);

    static constexpr size_t numberOfRecentTexts = 512;

    struct RecentText {
        uint32_t numberPlusOne;
        uint8_t length;
        std::array<uint8_t, 59> characters;
    };

    Lock lock;
    Vector<RecentText> recentTexts WTF_GUARDED_BY_LOCK(lock);
    HashMap<uint32_t, String, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> viewedTexts WTF_GUARDED_BY_LOCK(lock);
};

ImageText::ImageText(const Image& image)
    : m_endOfIdentifiers(image.header().numbers.endOfIdentifiers)
    , m_strings(image.section<ImageStringWord>(ImageSection::Strings).first(image.header().numbers.numberOfStringWords))
    , m_codedTexts(image.section<uint8_t>(ImageSection::Strings).subspan(m_strings.size_bytes()))
    , m_numberOfCodedTexts(image.header().numbers.numberOfCodedTexts)
    , m_decodedTexts(makeUnique<DecodedTexts>())
    , m_listedStrings(image.section<uint32_t>(ImageSection::LongStrings))
    , m_text(image.section<uint8_t>(ImageSection::Text))
    , m_identifiers(image.section<uint8_t>(ImageSection::Identifiers).first(image.header().numbers.endOfIdentifiers))
    , m_otherIdentifiers(spanReinterpretCast<const ImageOtherIdentifier>(image.section<uint8_t>(ImageSection::Identifiers).subspan(ImageOtherIdentifier::offsetFor(m_identifiers.size()))))
    , m_stringConstants(image.section<uint32_t>(ImageSection::StringConstants))
#if !ENABLE(AOT_POSITION_INDEPENDENT_IMAGE)
    , m_stringHeaders(image.section<ImageStringHeader>(ImageSection::StringHeaders).first(image.header().numbers.endOfStringHeaders))
    , m_addressOfText(image.header().address + image.header().sections[static_cast<unsigned>(ImageSection::Text)].offset)
{
}

ImageText::~ImageText() = default;
#else
    , m_stringRecords(image.section<ImageStringRecord>(ImageSection::StringHeaders))
{
    m_lazyHeaderOffsets = static_cast<std::atomic<uint32_t>*>(reserveGuardedPages(lazyHeaderOffsetsSize(), OSAllocator::UnknownUsage));
    m_lazyHeadersReservation = reserveGuardedPages(lazyHeadersReservationSize(), OSAllocator::UnknownUsage);
    m_lazyHeaders = std::bit_cast<uint8_t*>(roundUpToMultipleOf<lazyHeadersAlignment>(std::bit_cast<uintptr_t>(m_lazyHeadersReservation)));
    RELEASE_ASSERT(lazyHeadersCapacity() <= std::numeric_limits<uint32_t>::max());
    auto& arena = *reinterpret_cast<LazyStringHeaderArena*>(m_lazyHeaders);
    arena.end = sizeof(LazyStringHeaderArena);
}

ImageText::~ImageText()
{
    releaseGuardedPages(m_lazyHeaderOffsets, lazyHeaderOffsetsSize());
    releaseGuardedPages(m_lazyHeadersReservation, lazyHeadersReservationSize());
}

size_t ImageText::lazyHeadersReservationSize() const
{
    return roundUpToMultipleOf(pageSize(), lazyHeadersCapacity()) + lazyHeadersAlignment;
}

size_t ImageText::lazyHeaderOffsetsSize() const
{
    return roundUpToMultipleOf(pageSize(), std::max<size_t>(m_stringRecords.size(), 1) * sizeof(uint32_t));
}

namespace {

class LazyStringHeaderLocker {
    WTF_MAKE_NONCOPYABLE(LazyStringHeaderLocker);
public:
    explicit LazyStringHeaderLocker(uint8_t* lazyHeaders)
        : m_arena(*reinterpret_cast<LazyStringHeaderArena*>(lazyHeaders))
    {
        while (m_arena.isLocked.exchange(1, std::memory_order_acquire)) [[unlikely]]
            Thread::yield();
    }

    ~LazyStringHeaderLocker() { m_arena.isLocked.store(0, std::memory_order_release); }

    uint32_t& end() { return m_arena.end; }

private:
    LazyStringHeaderArena& m_arena;
};

} // anonymous namespace

size_t ImageText::lazyHeadersSize() const
{
    return LazyStringHeaderLocker(m_lazyHeaders).end();
}

NEVER_INLINE StringImpl& ImageText::createLazyHeader(uint32_t number) const
{
    const ImageStringRecord& record = m_stringRecords[number];
    RELEASE_ASSERT(record.hasText());
    ImageLongString range = rangeFor(record);
    ImageStringHeader header = ImageStringHeader::create(std::bit_cast<uint64_t>(m_text.data() + range.offset), range.length, record.hash(), record.is16Bit(), record.isAtom() ? StringImpl::StringAtom : StringImpl::StringNormal);
    LazyStringHeaderLocker locker(m_lazyHeaders);
    if (StringImpl* existing = tryGetLazyHeader(number)) [[unlikely]]
        return *existing;
    uint32_t offset = locker.end();
    locker.end() = offset + sizeof(LazyStringHeader);
    auto& created = *reinterpret_cast<LazyStringHeader*>(m_lazyHeaders + offset);
    created.header = header;
    created.number = number;
    m_lazyHeaderOffsets[number].store(offset, std::memory_order_release);
    return reinterpret_cast<StringImpl&>(created);
}

#endif

StringImpl* ImageText::headerOf(ImageStringWord word) const
{
    RELEASE_ASSERT_WITH_MESSAGE(word.hasText(), "AOT: the image has no text for this string because nothing referenced it at build time");
    RELEASE_ASSERT(word.hasHeader());
#if ENABLE(AOT_POSITION_INDEPENDENT_IMAGE)
    return m_stringRecords[word.number()].hasText() ? &ensureLazyHeader(word.number()) : nullptr;
#else
    const ImageStringHeader& header = headerAt(word.number());
    ASSERT(!header.length || header.refCount == StringImpl::refCountOfStaticString());
    return header.length ? reinterpret_cast<StringImpl*>(const_cast<ImageStringHeader*>(&header)) : nullptr;
#endif
}

uint32_t ImageText::textOfListedIdentifier(uint32_t number) const
{
    auto other = std::ranges::lower_bound(m_otherIdentifiers, number, { }, &ImageOtherIdentifier::number);
    return other != m_otherIdentifiers.end() && other->number == number ? other->textPlusOne : 0;
}

String ImageText::createStringOfCodedText(ImageStringWord word) const
{
    DecodedTexts& decoded = *m_decodedTexts;
    Locker locker { decoded.lock };
    if (decoded.recentTexts.isEmpty())
        decoded.recentTexts.fill(DecodedTexts::RecentText { }, DecodedTexts::numberOfRecentTexts);
    DecodedTexts::RecentText& recent = decoded.recentTexts[word.number() % DecodedTexts::numberOfRecentTexts];
    if (recent.numberPlusOne != word.number() + 1) {
        Vector<uint8_t, 64> text = CodedTexts(m_codedTexts, m_numberOfCodedTexts).at(word.number());
        if (text.size() > recent.characters.size())
            return String(byteCast<Latin1Character>(text.span()));
        recent.numberPlusOne = word.number() + 1;
        recent.length = static_cast<uint8_t>(text.size());
        std::ranges::copy(text, recent.characters.begin());
    }
    return String(byteCast<Latin1Character>(std::span<const uint8_t>(recent.characters).first(recent.length)));
}

NEVER_INLINE StringView ImageText::viewOfCodedText(ImageStringWord word) const
{
    DecodedTexts& decoded = *m_decodedTexts;
    Locker locker { decoded.lock };
    return decoded.viewedTexts.ensure(word.number(), [&] {
        Vector<uint8_t, 64> text = CodedTexts(m_codedTexts, m_numberOfCodedTexts).at(word.number());
        return String(byteCast<Latin1Character>(text.span()));
    }).iterator->value;
}

String ImageText::createStringWithoutHeader(ImageStringWord word) const
{
    if (word.isCoded())
        return createStringOfCodedText(word);
    StringView characters = view(word);
    if (characters.isEmpty())
        return emptyString();
    if (characters.is8Bit())
        return StringImpl::createWithoutCopying(characters.span8());
    return StringImpl::createWithoutCopying(characters.span16());
}

StringView ImageText::view(ImageStringWord word) const
{
    RELEASE_ASSERT_WITH_MESSAGE(word.hasText(), "AOT: the image has no text for this string because nothing referenced it at build time");
    ImageLongString range { 0, 0 };
    bool is16Bit = word.is16Bit();
    if (!word.hasHeader()) {
        if (word.isCoded()) [[unlikely]]
            return viewOfCodedText(word);
        range = ImageListedString::read(m_listedStrings.subspan(word.number()));
    }
#if ENABLE(AOT_POSITION_INDEPENDENT_IMAGE)
    else {
        const ImageStringRecord& record = m_stringRecords[word.number()];
        range = rangeFor(record);
        is16Bit = record.is16Bit();
    }
#else
    else if (const ImageStringHeader& header = headerAt(word.number()); header.length) {
        range = { static_cast<uint32_t>(header.characters - m_addressOfText), header.length };
        is16Bit = !header.is8Bit();
    }
#endif
    if (is16Bit)
        return StringView(spanReinterpretCast<const char16_t>(m_text.subspan(range.offset, range.length * sizeof(char16_t))));
    return StringView(byteCast<Latin1Character>(m_text.subspan(range.offset, range.length)));
}

unsigned ImageText::hash(ImageStringWord word) const
{
#if !ENABLE(AOT_POSITION_INDEPENDENT_IMAGE)
    if (word.hasIdentifierNumber()) {
        const ImageStringHeader& header = headerAt(word.number());
        return header.length ? header.hash() : StringImpl::empty()->hash();
    }
#endif
    if (word.hasListedHash()) {
        std::span<const uint32_t> words = m_listedStrings.subspan(word.number());
        return words[ImageListedString::numberOfWords(ImageListedString::read(words))];
    }
    if (!word.hasHeader())
        return view(word).hash();
#if ENABLE(AOT_POSITION_INDEPENDENT_IMAGE)
    const ImageStringRecord& record = m_stringRecords[word.number()];
    return record.hasText() ? record.hash() : StringImpl::empty()->hash();
#else
    return headerAt(word.number()).hash();
#endif
}

bool ImageText::equals(StringID string, StringView text) const
{
    return hasText(string) && view(string) == text;
}

String ImageText::createString(ImageStringWord word) const
{
    if (!hasHeader(word))
        return createStringWithoutHeader(word);
    StringImpl* header = headerOf(word);
    return header ? String(*header) : emptyString();
}

String ImageText::createAtom(ImageStringWord word) const
{
    if (!hasHeader(word))
        return AtomString(createStringWithoutHeader(word)).string();
    StringImpl* header = headerOf(word);
    if (!header)
        return emptyString();
    if (header->isAtom())
        return String(*header);
    return AtomStringImpl::add(header);
}

static const ImageHeader& headerOfImage(std::span<const uint8_t> image)
{
    RELEASE_ASSERT(image.size() >= sizeof(ImageHeader));
    return *reinterpret_cast<const ImageHeader*>(image.data());
}

static ImageSectionRange rangeOfStringHeaders(std::span<const uint8_t> image)
{
    ImageSectionRange range = headerOfImage(image).sections[static_cast<unsigned>(ImageSection::StringHeaders)];
    RELEASE_ASSERT(static_cast<uint64_t>(range.offset) + range.size <= image.size());
    return range;
}

void relocateImage(std::span<uint8_t> image, uint64_t address)
{
    if (headerOfImage(image).options.form() == ImageForm::PositionIndependent)
        return;
    uint64_t distance = address - headerOfImage(image).address;
    if (!distance)
        return;
    ImageSectionRange range = rangeOfStringHeaders(image);
    for (ImageStringHeader& header : spanReinterpretCast<ImageStringHeader>(image.subspan(range.offset, range.size))) {
        if (header.characters)
            header.characters += distance;
    }
    reinterpret_cast<ImageHeader*>(image.data())->address = address;
}

#if !ENABLE(AOT_POSITION_INDEPENDENT_IMAGE)
static std::span<uint8_t> pagesHolding(std::span<const uint8_t> bytes)
{
    uintptr_t start = std::bit_cast<uintptr_t>(bytes.data());
    uintptr_t firstPage = roundDownToMultipleOf(WTF::pageSize(), start);
    uintptr_t end = roundUpToMultipleOf(WTF::pageSize(), start + bytes.size());
    return { std::bit_cast<uint8_t*>(firstPage), end - firstPage };
}

#if OS(WINDOWS)

static bool allowWrites(std::span<uint8_t> pages)
{
    DWORD previousProtection;
    return VirtualProtect(pages.data(), pages.size(), PAGE_READWRITE, &previousProtection);
}

static void forbidWrites(std::span<uint8_t> pages)
{
    DWORD previousProtection;
    VirtualProtect(pages.data(), pages.size(), PAGE_READONLY, &previousProtection);
}

#else

static bool allowWrites(std::span<uint8_t> pages)
{
    if (!mprotect(pages.data(), pages.size(), PROT_READ | PROT_WRITE))
        return true;
    Vector<uint8_t> contents(std::span<const uint8_t> { pages });
    if (mmap(pages.data(), pages.size(), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0) == MAP_FAILED)
        return false;
    memcpySpan(pages, contents.span());
    return true;
}

static void forbidWrites(std::span<uint8_t> pages)
{
    mprotect(pages.data(), pages.size(), PROT_READ);
}
#endif

bool relocateLoadedImage(std::span<const uint8_t> image)
{
    uintptr_t address = std::bit_cast<uintptr_t>(image.data());
    if (headerOfImage(image).address == address) [[likely]]
        return true;

    ImageSectionRange range = rangeOfStringHeaders(image);
    std::array<std::span<uint8_t>, 2> writtenPages { pagesHolding(image.first(sizeof(ImageHeader))), pagesHolding(image.subspan(range.offset, range.size)) };
    for (std::span<uint8_t> pages : writtenPages) {
        if (!pages.empty() && !allowWrites(pages))
            return false;
    }
    relocateImage({ const_cast<uint8_t*>(image.data()), image.size() }, address);
    if (address % WTF::pageSize())
        return true;
    for (std::span<uint8_t> pages : writtenPages) {
        if (!pages.empty())
            forbidWrites(pages);
    }
    return true;
}
#endif

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
