/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTImageForward.h"
#include "JSExportMacros.h"
#include <atomic>
#include <cstddef>
#include <optional>
#include <span>
#include <wtf/Noncopyable.h>
#include <wtf/text/StringImpl.h>
#include <wtf/text/StringView.h>
#include <wtf/text/UniquedStringImpl.h>
#include <wtf/text/WTFString.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

struct ImageLongString {
    uint32_t offset;
    uint32_t length;
};
static_assert(sizeof(ImageLongString) == 8 && isImageRecord<ImageLongString>);

struct ImageStringWord;

struct ImageString {
    static constexpr unsigned lengthBits = 8;
    static constexpr uint32_t lengthMask = (1u << lengthBits) - 1;
    static constexpr uint32_t hashMask = (1u << 24) - 1;
    static constexpr uint32_t is16BitFlag = 1u << 24;
    static constexpr uint32_t isAtomFlag = 1u << 25;
    static constexpr uint32_t hasIdentifierNumberFlag = 1u << 26;
    static constexpr uint32_t hasNoHeaderFlag = 1u << 27;
    static constexpr uint32_t hasTextFlag = 1u << 28;
    static constexpr uint32_t hasListedHashFlag = 1u << 29;

    bool hasText() const { return !!location; }
    bool is16Bit() const { return hashAndFlags & is16BitFlag; }
    bool isAtom() const { return hashAndFlags & isAtomFlag; }
    bool hasIdentifierNumber() const { return hashAndFlags & hasIdentifierNumberFlag; }
    bool hasHeader() const { return hasText() && !(hashAndFlags & hasNoHeaderFlag); }
    bool hasListedHash() const { return hashAndFlags & hasListedHashFlag; }
    uint32_t hashOrIdentifierNumber() const { return hashAndFlags & hashMask; }
    ImageStringWord inImage(uint32_t number) const;
    ImageLongString range(std::span<const ImageLongString> longStrings) const;

    uint32_t location;
    uint32_t hashAndFlags;
};
static_assert(sizeof(ImageString) == 8 && isImageRecord<ImageString>);

struct ImageStringWord {
    bool hasText() const { return !!bits; }
    bool is16Bit() const { return bits & ImageString::is16BitFlag; }
    bool isAtom() const { return bits & ImageString::isAtomFlag; }
    bool hasIdentifierNumber() const { return bits & ImageString::hasIdentifierNumberFlag; }
    bool hasHeader() const { return hasText() && !(bits & ImageString::hasNoHeaderFlag); }
    bool hasListedHash() const { return bits & ImageString::hasListedHashFlag; }
    uint32_t number() const { return bits & ImageString::hashMask; }

    uint32_t bits;
};
static_assert(sizeof(ImageStringWord) == 4 && isImageRecord<ImageStringWord>);

inline ImageStringWord ImageString::inImage(uint32_t number) const
{
    if (!hasText())
        return { 0 };
    uint32_t flags = (hashAndFlags & ~hashMask) | hasTextFlag;
    if (hasIdentifierNumber())
        return { flags | hashOrIdentifierNumber() };
    return { flags | number };
}

// ImageSection::LongStrings in an image: where the strings without a header are. One word, the offset above the length, or a zero word, the offset and the length.
struct ImageListedString {
    static constexpr unsigned maxNumberOfWords = 3;

    static bool fitsOneWord(ImageLongString range) { return range.length && range.length <= ImageString::lengthMask && !(range.offset >> (32 - ImageString::lengthBits)); }
    static unsigned numberOfWords(ImageLongString range) { return fitsOneWord(range) ? 1 : maxNumberOfWords; }
    static void write(std::span<uint32_t> words, ImageLongString range)
    {
        if (fitsOneWord(range)) {
            words[0] = range.offset << ImageString::lengthBits | range.length;
            return;
        }
        words[0] = 0;
        words[1] = range.offset;
        words[2] = range.length;
    }
    static ImageLongString read(std::span<const uint32_t> words)
    {
        if (uint32_t length = words[0] & ImageString::lengthMask)
            return { words[0] >> ImageString::lengthBits, length };
        return { words[1], words[2] };
    }
};

inline ImageLongString ImageString::range(std::span<const ImageLongString> longStrings) const
{
    uint32_t length = location & lengthMask;
    uint32_t offsetOrIndex = location >> lengthBits;
    if (!length)
        return longStrings[offsetOrIndex];
    return { offsetOrIndex, length };
}

struct ImageStringRecord {
    static constexpr uint32_t isLiteralStringFlag = 1u << 30;

    static ImageStringRecord create(uint32_t rangeOrIndex, uint32_t hash, bool is16Bit, bool isAtom)
    {
        return { rangeOrIndex, hash | ImageString::hasTextFlag | (is16Bit ? ImageString::is16BitFlag : 0) | (isAtom ? ImageString::isAtomFlag : 0) };
    }

    bool hasText() const { return !!hashAndFlags; }
    bool is16Bit() const { return hashAndFlags & ImageString::is16BitFlag; }
    bool isAtom() const { return hashAndFlags & ImageString::isAtomFlag; }
    bool isLiteralString() const { return hashAndFlags & isLiteralStringFlag; }
    uint32_t hash() const { return hashAndFlags & ImageString::hashMask; }

    uint32_t rangeOrIndex;
    uint32_t hashAndFlags;
};
static_assert(sizeof(ImageStringRecord) == 8 && isImageRecord<ImageStringRecord>);

// The words of a WTF::StaticStringImpl.
struct ImageStringHeader {
    static ImageStringHeader create(uint64_t characters, uint32_t length, uint32_t hash, bool is16Bit, StringImpl::StringKind kind)
    {
        return { StringImpl::refCountOfStaticString(), length, characters, StringImpl::hashAndFlagsOfStaticString(hash, !is16Bit, kind), 0 };
    }

    uint32_t hash() const { return hashAndFlags >> StringImpl::s_flagCount; }

    uint32_t refCount;
    uint32_t length;
    uint64_t characters;
    uint32_t hashAndFlags;
    uint32_t literalStringWord;
};
static_assert(sizeof(ImageStringHeader) == sizeof(StringImpl) && alignof(ImageStringHeader) == alignof(StringImpl) && isImageRecord<ImageStringHeader>);
static_assert(offsetof(ImageStringHeader, length) == static_cast<size_t>(StringImpl::lengthMemoryOffset()));
static_assert(offsetof(ImageStringHeader, characters) == StringImpl::dataOffset());
static_assert(offsetof(ImageStringHeader, hashAndFlags) == StringImpl::flagsOffset());

struct LazyStringHeader {
    ImageStringHeader header;
    uint32_t number;
    uint32_t unused;
};
static_assert(sizeof(LazyStringHeader) == 32 && !offsetof(LazyStringHeader, header));

struct LazyStringHeaderArena {
    uint64_t unused;
    uint32_t end;
    std::atomic<uint32_t> isLocked;
    uint32_t unused2;
    uint32_t sizeInBytes;
    uint64_t unused3;
};
static_assert(sizeof(LazyStringHeaderArena) == sizeof(LazyStringHeader) && offsetof(LazyStringHeaderArena, sizeInBytes) == offsetof(ImageStringHeader, literalStringWord));

struct ImageIdentifierWord;

struct ImageIdentifier {
    static constexpr uint32_t hashMask = (1u << 24) - 1;
    static constexpr unsigned kindShift = 24;
    static constexpr unsigned kindBits = 3;
    static constexpr unsigned nameClassShift = kindShift + kindBits;
    static constexpr unsigned nameClassBits = 5;

    static constexpr ImageIdentifier create(StringID text, uint32_t hashOrSymbol, PropertyKeyKind kind, NameClass nameClass)
    {
        RELEASE_ASSERT_UNDER_CONSTEXPR_CONTEXT(hashOrSymbol <= hashMask);
        RELEASE_ASSERT_UNDER_CONSTEXPR_CONTEXT(static_cast<uint32_t>(kind) < 1u << kindBits && static_cast<uint32_t>(nameClass) < 1u << nameClassBits);
        return { text ? text.index() + 1 : 0, hashOrSymbol | static_cast<uint32_t>(kind) << kindShift | static_cast<uint32_t>(nameClass) << nameClassShift };
    }

    StringID text() const { return textPlusOne ? StringID(textPlusOne - 1) : StringID(); }
    uint32_t hashOrSymbol() const { return bits & hashMask; }
    PropertyKeyKind kind() const { return static_cast<PropertyKeyKind>(bits >> kindShift & ((1u << kindBits) - 1)); }
    NameClass nameClass() const { return static_cast<NameClass>(bits >> nameClassShift); }
    ImageIdentifierWord inImage() const;

    uint32_t textPlusOne;
    uint32_t bits;
};
static_assert(sizeof(ImageIdentifier) == 8 && isImageRecord<ImageIdentifier>);

struct ImageIdentifierWord {
    static constexpr uint32_t textMask = ImageIdentifier::hashMask;

    StringID text() const { return bits & textMask ? StringID((bits & textMask) - 1) : StringID(); }
    PropertyKeyKind kind() const { return static_cast<PropertyKeyKind>(bits >> ImageIdentifier::kindShift & ((1u << ImageIdentifier::kindBits) - 1)); }
    NameClass nameClass() const { return kind() == PropertyKeyKind::WellKnownSymbol ? NameClass::Ordinary : static_cast<NameClass>(bits >> ImageIdentifier::nameClassShift); }
    Typed::WellKnownSymbol wellKnownSymbol() const
    {
        ASSERT(kind() == PropertyKeyKind::WellKnownSymbol);
        return static_cast<Typed::WellKnownSymbol>(bits >> ImageIdentifier::nameClassShift);
    }

    uint32_t bits;
};
static_assert(sizeof(ImageIdentifierWord) == 4 && isImageRecord<ImageIdentifierWord>);
static_assert(numberOfEnumerators<Typed::WellKnownSymbol> <= 1u << ImageIdentifier::nameClassBits);

inline ImageIdentifierWord ImageIdentifier::inImage() const
{
    RELEASE_ASSERT(textPlusOne <= ImageIdentifierWord::textMask);
    uint32_t kindBitsInPlace = static_cast<uint32_t>(kind()) << kindShift;
    if (kind() == PropertyKeyKind::WellKnownSymbol) {
        RELEASE_ASSERT(nameClass() == NameClass::Ordinary);
        return { textPlusOne | kindBitsInPlace | hashOrSymbol() << nameClassShift };
    }
    return { textPlusOne | kindBitsInPlace | static_cast<uint32_t>(nameClass()) << nameClassShift };
}

class ImageText {
#if ENABLE(AOT_POSITION_INDEPENDENT_IMAGE)
    WTF_MAKE_NONCOPYABLE(ImageText);
#endif
public:
    JS_EXPORT_PRIVATE explicit ImageText(const Image&);
#if ENABLE(AOT_POSITION_INDEPENDENT_IMAGE)
    JS_EXPORT_PRIVATE ~ImageText();
#endif

    uint32_t numberOfStrings() const { return static_cast<uint32_t>(m_strings.size()); }
    uint32_t numberOfIdentifiers() const { return static_cast<uint32_t>(m_identifiers.size()); }
    uint32_t numberOfStringConstants() const { return static_cast<uint32_t>(m_stringConstants.size()); }

    bool hasText(StringID string) const { return m_strings[string.index()].hasText(); }
    bool isAtom(StringID string) const { return m_strings[string.index()].isAtom(); }
    JS_EXPORT_PRIVATE StringView view(StringID) const;
    JS_EXPORT_PRIVATE unsigned hash(StringID) const;
    JS_EXPORT_PRIVATE bool equals(StringID, StringView) const;
    JS_EXPORT_PRIVATE String createString(StringID) const;
    JS_EXPORT_PRIVATE String createAtom(StringID) const;
    StringImpl* tryGetHeader(StringID) const;

    std::optional<uint32_t> identifierNumber(StringID) const;
    const ImageIdentifierWord& identifier(uint32_t number) const { return m_identifiers[number]; }
#if ENABLE(AOT_POSITION_INDEPENDENT_IMAGE)
    std::span<const ImageStringRecord> stringRecords() const { return m_stringRecords; }
    StringView recordView(uint32_t number) const { return view(m_stringRecords[number]); }
    StringView view(const ImageStringRecord&) const;
    StringImpl* tryGetLazyHeader(uint32_t number) const;
    StringImpl& ensureLazyHeader(uint32_t number) const;
    std::optional<uint32_t> lazyHeaderNumber(const StringImpl&) const;
    const uint32_t* lazyHeaderOffsets() const { return reinterpret_cast<const uint32_t*>(m_lazyHeaderOffsets); }
    uint8_t* lazyHeaders() const { return m_lazyHeaders; }
    size_t lazyHeadersCapacity() const { return (m_stringRecords.size() + 1) * sizeof(LazyStringHeader); }
    JS_EXPORT_PRIVATE size_t lazyHeadersSize() const;
#else
    UniquedStringImpl* identifierAtoms() const { return reinterpret_cast<UniquedStringImpl*>(const_cast<ImageStringHeader*>(m_stringHeaders.data())); }
#endif
    StringID stringConstant(uint32_t number) const { return StringID(m_stringConstants[number]); }
    bool hasListedHash(StringID string) const { return m_strings[string.index()].hasListedHash(); }
    std::optional<uint32_t> literalStringWordOfHeader(const StringImpl&) const;

private:
    bool hasHeader(StringID string) const { return !m_strings[string.index()].hasText() || m_strings[string.index()].hasHeader(); }
    StringImpl* headerOf(StringID) const;
#if ENABLE(AOT_POSITION_INDEPENDENT_IMAGE)
    ImageLongString rangeFor(const ImageStringRecord&) const;
    uint32_t literalStringWordFor(const ImageStringRecord&) const;
    JS_EXPORT_PRIVATE StringImpl& createLazyHeader(uint32_t number) const;
    size_t lazyHeaderOffsetsSize() const;
    size_t lazyHeadersReservationSize() const;

    static constexpr size_t lazyHeadersAlignment = 2 * MB;
#endif
    String createStringWithoutHeader(StringID) const;

    std::span<const ImageStringWord> m_strings;
    std::span<const uint32_t> m_listedStrings;
    std::span<const uint8_t> m_text;
    std::span<const ImageIdentifierWord> m_identifiers;
    std::span<const uint32_t> m_stringConstants;
#if ENABLE(AOT_POSITION_INDEPENDENT_IMAGE)
    std::span<const ImageStringRecord> m_stringRecords;
    std::atomic<uint32_t>* m_lazyHeaderOffsets { nullptr };
    uint8_t* m_lazyHeaders { nullptr };
    void* m_lazyHeadersReservation { nullptr };
#else
    std::span<const ImageStringHeader> m_stringHeaders;
    uint64_t m_addressOfText { 0 };
#endif
};

#if ENABLE(AOT_POSITION_INDEPENDENT_IMAGE)
inline ImageLongString ImageText::rangeFor(const ImageStringRecord& record) const
{
    if (uint32_t length = record.rangeOrIndex & ImageString::lengthMask) [[likely]]
        return { record.rangeOrIndex >> ImageString::lengthBits, length };
    if (!record.hasText())
        return { 0, 0 };
    return ImageListedString::read(m_listedStrings.subspan(record.rangeOrIndex >> ImageString::lengthBits));
}

inline StringView ImageText::view(const ImageStringRecord& record) const
{
    ImageLongString range = rangeFor(record);
    const uint8_t* characters = m_text.data() + range.offset;
    if (record.is16Bit())
        return StringView(std::span { reinterpret_cast<const char16_t*>(characters), range.length });
    return StringView(std::span { byteCast<Latin1Character>(characters), range.length });
}

inline StringImpl* ImageText::tryGetLazyHeader(uint32_t number) const
{
    uint32_t offset = m_lazyHeaderOffsets[number].load(std::memory_order_acquire);
    return offset ? reinterpret_cast<StringImpl*>(m_lazyHeaders + offset) : nullptr;
}

inline StringImpl& ImageText::ensureLazyHeader(uint32_t number) const
{
    if (StringImpl* header = tryGetLazyHeader(number)) [[likely]]
        return *header;
    return createLazyHeader(number);
}

inline std::optional<uint32_t> ImageText::lazyHeaderNumber(const StringImpl& string) const
{
    uintptr_t offset = std::bit_cast<uintptr_t>(&string) - std::bit_cast<uintptr_t>(m_lazyHeaders);
    if (offset >= lazyHeadersCapacity())
        return std::nullopt;
    return reinterpret_cast<const LazyStringHeader&>(string).number;
}
#endif

inline std::optional<uint32_t> ImageText::literalStringWordOfHeader(const StringImpl& string) const
{
#if ENABLE(AOT_POSITION_INDEPENDENT_IMAGE)
    if (!lazyHeaderNumber(string))
#else
    uintptr_t offset = std::bit_cast<uintptr_t>(&string) - std::bit_cast<uintptr_t>(m_stringHeaders.data());
    if (offset >= m_stringHeaders.size_bytes())
#endif
        return std::nullopt;
    return reinterpret_cast<const ImageStringHeader&>(string).literalStringWord;
}

inline StringImpl* ImageText::tryGetHeader(StringID string) const
{
    ImageStringWord word = m_strings[string.index()];
    if (!word.hasHeader())
        return nullptr;
#if ENABLE(AOT_POSITION_INDEPENDENT_IMAGE)
    if (rangeFor(m_stringRecords[word.number()]).length < 2)
        return nullptr;
    if (word.isAtom() && !word.hasIdentifierNumber())
        return nullptr;
    return &ensureLazyHeader(word.number());
#else
    const ImageStringHeader& header = m_stringHeaders[word.number()];
    if (header.length < 2)
        return nullptr;
    auto* impl = reinterpret_cast<StringImpl*>(const_cast<ImageStringHeader*>(&header));
    if (word.isAtom() && !impl->isAtom())
        return nullptr;
    return impl;
#endif
}

inline std::optional<uint32_t> ImageText::identifierNumber(StringID string) const
{
    ImageStringWord word = m_strings[string.index()];
    if (!word.hasIdentifierNumber())
        return std::nullopt;
    return word.number();
}

// An image holds addresses in two kinds of places, and nowhere else: ImageHeader::address, which is the address of the image itself, and ImageStringHeader::characters.
// A position-independent image holds none.
struct ImageAddressPlaces {
    uint64_t offsetOfImageAddress;
    uint64_t offsetOfFirstStringAddress;
    uint64_t bytesBetweenStringAddresses;
    uint64_t numberOfStringAddresses;
};
JS_EXPORT_PRIVATE ImageAddressPlaces addressPlacesOf(std::span<const uint8_t> image);
JS_EXPORT_PRIVATE void relocateImage(std::span<uint8_t> image, uint64_t address);

#if ENABLE(AOT_POSITION_INDEPENDENT_IMAGE)
[[nodiscard]] inline bool relocateLoadedImage(std::span<const uint8_t>) { return true; }
#else
[[nodiscard]] JS_EXPORT_PRIVATE bool relocateLoadedImage(std::span<const uint8_t> image);
#endif

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
