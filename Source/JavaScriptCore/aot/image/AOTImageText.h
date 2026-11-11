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
#include <memory>
#include <wtf/Noncopyable.h>
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
    static constexpr uint32_t isShorterThanTwoFlag = 1u << 30;
    static constexpr uint32_t isCodedFlag = 1u << 31;

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
    static constexpr ImageStringWord ofIdentifier(uint32_t number) { return { ImageString::hasTextFlag | ImageString::isAtomFlag | ImageString::hasIdentifierNumberFlag | number }; }

    bool hasText() const { return !!bits; }
    bool is16Bit() const { return bits & ImageString::is16BitFlag; }
    bool isAtom() const { return bits & ImageString::isAtomFlag; }
    bool hasIdentifierNumber() const { return bits & ImageString::hasIdentifierNumberFlag; }
    bool hasHeader() const { return hasText() && !(bits & ImageString::hasNoHeaderFlag); }
    bool hasListedHash() const { return bits & ImageString::hasListedHashFlag; }
    bool isCoded() const { return bits & ImageString::isCodedFlag; }
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
    static ImageStringRecord create(uint32_t rangeOrIndex, uint32_t hash, bool is16Bit, bool isAtom)
    {
        return { rangeOrIndex, hash | ImageString::hasTextFlag | (is16Bit ? ImageString::is16BitFlag : 0) | (isAtom ? ImageString::isAtomFlag : 0) };
    }

    bool hasText() const { return !!hashAndFlags; }
    bool is16Bit() const { return hashAndFlags & ImageString::is16BitFlag; }
    bool isAtom() const { return hashAndFlags & ImageString::isAtomFlag; }
    uint32_t hash() const { return hashAndFlags & ImageString::hashMask; }

    uint32_t rangeOrIndex;
    uint32_t hashAndFlags;
};
static_assert(sizeof(ImageStringRecord) == 8 && isImageRecord<ImageStringRecord>);

struct ImageStringHeader {
    static ImageStringHeader create(uint64_t characters, uint32_t length, uint32_t hash, bool is16Bit, StringImpl::StringKind kind)
    {
        return { StringImpl::refCountOfStaticString(), length, characters, StringImpl::hashAndFlagsOfStaticString(hash, !is16Bit, kind), 0 };
    }

    uint32_t hash() const { return hashAndFlags >> StringImpl::s_flagCount; }
    bool is8Bit() const { return hashAndFlags & StringImpl::flagIs8Bit(); }

    uint32_t refCount;
    uint32_t length;
    uint64_t characters;
    uint32_t hashAndFlags;
    uint32_t unused;
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
    uint64_t unused2;
    uint64_t unused3;
};
static_assert(sizeof(LazyStringHeaderArena) == sizeof(LazyStringHeader));

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
    ImageIdentifierWord word(uint32_t textNumberPlusOne) const;

    uint32_t textPlusOne;
    uint32_t bits;
};
static_assert(sizeof(ImageIdentifier) == 8 && isImageRecord<ImageIdentifier>);

struct ImageIdentifierWord {
    static constexpr uint32_t textMask = ImageIdentifier::hashMask;

    uint8_t byteInImage() const { return bits >> ImageIdentifier::kindShift; }
    static ImageIdentifierWord ofByteInImage(uint8_t byte) { return { static_cast<uint32_t>(byte) << ImageIdentifier::kindShift }; }

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
static_assert(sizeof(ImageIdentifierWord) == 4 && ImageIdentifier::kindShift == 24);

struct ImageOtherIdentifier {
    static constexpr size_t offsetFor(size_t numberOfIdentifiers) { return (numberOfIdentifiers + sizeof(uint32_t) - 1) / sizeof(uint32_t) * sizeof(uint32_t); }
    static constexpr bool isSizeOfSection(size_t size, size_t numberOfIdentifiers) { return size >= offsetFor(numberOfIdentifiers) && !((size - offsetFor(numberOfIdentifiers)) % (2 * sizeof(uint32_t))); }

    uint32_t number;
    uint32_t textPlusOne;
};
static_assert(sizeof(ImageOtherIdentifier) == 8 && isImageRecord<ImageOtherIdentifier>);
static_assert(numberOfEnumerators<Typed::WellKnownSymbol> <= 1u << ImageIdentifier::nameClassBits);

inline ImageIdentifierWord ImageIdentifier::word(uint32_t textNumberPlusOne) const
{
    RELEASE_ASSERT(textNumberPlusOne <= ImageIdentifierWord::textMask);
    uint32_t kindBitsInPlace = static_cast<uint32_t>(kind()) << kindShift;
    if (kind() == PropertyKeyKind::WellKnownSymbol) {
        RELEASE_ASSERT(nameClass() == NameClass::Ordinary);
        return { textNumberPlusOne | kindBitsInPlace | hashOrSymbol() << nameClassShift };
    }
    return { textNumberPlusOne | kindBitsInPlace | static_cast<uint32_t>(nameClass()) << nameClassShift };
}

class ImageText {
    WTF_MAKE_NONCOPYABLE(ImageText);
public:
    JS_EXPORT_PRIVATE explicit ImageText(const Image&);
    JS_EXPORT_PRIVATE ~ImageText();

    uint32_t numberOfStrings() const { return endOfStringsWithWords() + m_numberOfCodedTexts; }
    uint32_t endOfStringsWithWords() const { return m_endOfIdentifiers + static_cast<uint32_t>(m_strings.size()); }
    uint32_t numberOfIdentifiers() const { return static_cast<uint32_t>(m_identifiers.size()); }
    uint32_t numberOfStringConstants() const { return static_cast<uint32_t>(m_stringConstants.size()); }

    bool hasText(StringID string) const { return wordOf(string).hasText(); }
    bool isAtom(StringID string) const { return wordOf(string).isAtom(); }
    ImageStringWord wordOf(StringID string) const
    {
        uint32_t number = string.index();
        if (number < m_endOfIdentifiers) [[likely]]
            return { number ? ImageStringWord::ofIdentifier(number).bits : 0 };
        if (number < endOfStringsWithWords())
            return m_strings[number - m_endOfIdentifiers];
        return { ImageString::hasTextFlag | ImageString::hasNoHeaderFlag | ImageString::isCodedFlag | (number - endOfStringsWithWords()) };
    }
    StringView view(StringID string) const { return view(wordOf(string)); }
    ALWAYS_INLINE StringView viewInline(StringID string) const
    {
        ImageStringWord word = wordOf(string);
#if !ENABLE(AOT_POSITION_INDEPENDENT_IMAGE)
        if ((word.bits & (ImageString::hasTextFlag | ImageString::hasNoHeaderFlag)) == ImageString::hasTextFlag) [[likely]] {
            const ImageStringHeader& header = headerAt(word.number());
            if (header.length && header.is8Bit()) [[likely]]
                return StringView(unsafeMakeSpan(std::bit_cast<const Latin1Character*>(header.characters), header.length));
        }
#endif
        return view(word);
    }
    unsigned hash(StringID string) const { return hash(wordOf(string)); }
    JS_EXPORT_PRIVATE bool equals(StringID, StringView) const;
    String createString(StringID string) const { return createString(wordOf(string)); }
    String createAtom(StringID string) const { return createAtom(wordOf(string)); }
    StringImpl* tryGetHeader(StringID string) const { return tryGetHeader(wordOf(string)); }
    JS_EXPORT_PRIVATE StringView view(ImageStringWord) const;
    JS_EXPORT_PRIVATE unsigned hash(ImageStringWord) const;
    JS_EXPORT_PRIVATE String createString(ImageStringWord) const;
    JS_EXPORT_PRIVATE String createAtom(ImageStringWord) const;
    StringImpl* tryGetHeader(ImageStringWord) const;

    ImageIdentifierWord identifier(uint32_t number) const
    {
        ImageIdentifierWord word = ImageIdentifierWord::ofByteInImage(m_identifiers[number]);
        word.bits |= word.kind() == PropertyKeyKind::Text ? (number ? number + 1 : 0) : textOfListedIdentifier(number);
        return word;
    }
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
    ImageStringWord stringConstant(uint32_t number) const { return { m_stringConstants[number] }; }
    bool hasListedHash(StringID string) const { return wordOf(string).hasListedHash(); }

private:
    static bool hasHeader(ImageStringWord word) { return !word.hasText() || word.hasHeader(); }
    StringImpl* headerOf(ImageStringWord) const;
#if ENABLE(AOT_POSITION_INDEPENDENT_IMAGE)
    ImageLongString rangeFor(const ImageStringRecord&) const;
    JS_EXPORT_PRIVATE StringImpl& createLazyHeader(uint32_t number) const;
    size_t lazyHeaderOffsetsSize() const;
    size_t lazyHeadersReservationSize() const;

    static constexpr size_t lazyHeadersAlignment = 2 * MB;
#endif
    String createStringWithoutHeader(ImageStringWord) const;
    String createStringOfCodedText(ImageStringWord) const;
    JS_EXPORT_PRIVATE uint32_t textOfListedIdentifier(uint32_t number) const;
    StringView viewOfCodedText(ImageStringWord) const;

    struct DecodedTexts;

    const uint32_t m_endOfIdentifiers;
    std::span<const ImageStringWord> m_strings;
    std::span<const uint8_t> m_codedTexts;
    uint32_t m_numberOfCodedTexts { 0 };
    const std::unique_ptr<DecodedTexts> m_decodedTexts;
    std::span<const uint32_t> m_listedStrings;
    std::span<const uint8_t> m_text;
    std::span<const uint8_t> m_identifiers;
    std::span<const ImageOtherIdentifier> m_otherIdentifiers;
    std::span<const uint32_t> m_stringConstants;
#if ENABLE(AOT_POSITION_INDEPENDENT_IMAGE)
    std::span<const ImageStringRecord> m_stringRecords;
    std::atomic<uint32_t>* m_lazyHeaderOffsets { nullptr };
    uint8_t* m_lazyHeaders { nullptr };
    void* m_lazyHeadersReservation { nullptr };
#else
    const ImageStringHeader& headerAt(uint32_t number) const { return m_stringHeaders.data()[number]; }

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

inline StringImpl* ImageText::tryGetHeader(ImageStringWord word) const
{
    if (!word.hasHeader())
        return nullptr;
#if ENABLE(AOT_POSITION_INDEPENDENT_IMAGE)
    if (rangeFor(m_stringRecords[word.number()]).length < 2)
        return nullptr;
    if (word.isAtom() && !word.hasIdentifierNumber())
        return nullptr;
    return &ensureLazyHeader(word.number());
#else
    const ImageStringHeader& header = headerAt(word.number());
    if (header.length < 2)
        return nullptr;
    auto* impl = reinterpret_cast<StringImpl*>(const_cast<ImageStringHeader*>(&header));
    if (word.isAtom() && !impl->isAtom())
        return nullptr;
    return impl;
#endif
}

JS_EXPORT_PRIVATE void relocateImage(std::span<uint8_t> image, uint64_t address);

#if ENABLE(AOT_POSITION_INDEPENDENT_IMAGE)
[[nodiscard]] inline bool relocateLoadedImage(std::span<const uint8_t>) { return true; }
#else
[[nodiscard]] JS_EXPORT_PRIVATE bool relocateLoadedImage(std::span<const uint8_t> image);
#endif

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
