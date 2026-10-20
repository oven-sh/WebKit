/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTImageText.h"

#if ENABLE(AOT)

#include "AOTImage.h"
#include <wtf/StdLibExtras.h>
#include <wtf/text/AtomStringImpl.h>
#include <wtf/text/StringHasherInlines.h>
#include <wtf/text/StringImpl.h>

namespace JSC { namespace AOT {

static_assert(ImageString::hashMask == StringHasher::maskHash && ImageIdentifier::hashMask == StringHasher::maskHash);

static ImageLongString decodeLocation(uint32_t location, std::span<const ImageLongString> longStrings)
{
    uint32_t length = location & ImageString::lengthMask;
    uint32_t offsetOrIndex = location >> ImageString::lengthBits;
    if (!length)
        return longStrings[offsetOrIndex];
    return { offsetOrIndex, length };
}

template<typename CharacterType>
static String createAtomWithoutCopying(std::span<const CharacterType> characters, unsigned hash)
{
    WTF::HashTranslatorCharBuffer<CharacterType> buffer { characters, hash };
    RELEASE_ASSERT(AtomStringImpl::isValidPrecomputedHash(buffer.hash));
    ASSERT(buffer.hash == StringHasher::computeHashAndMaskTop8Bits(buffer.characters));
    return AtomStringImpl::addWithoutCopying(buffer);
}

ImageText::ImageText(const Image& image)
    : m_strings(image.section<ImageString>(ImageSection::Strings))
    , m_longStrings(image.section<ImageLongString>(ImageSection::LongStrings))
    , m_text(image.section<uint8_t>(ImageSection::Text))
    , m_identifiers(image.section<ImageIdentifier>(ImageSection::Identifiers))
    , m_stringConstants(image.section<uint32_t>(ImageSection::StringConstants))
{
}

StringView ImageText::view(StringID string) const
{
    const ImageString& record = m_strings[string.index()];
    RELEASE_ASSERT_WITH_MESSAGE(record.hasText(), "AOT: the image has no text for this string because nothing referenced it at build time");
    ImageLongString range = decodeLocation(record.location, m_longStrings);
    if (record.is16Bit())
        return StringView(spanReinterpretCast<const char16_t>(m_text.subspan(range.offset, range.length * sizeof(char16_t))));
    return StringView(byteCast<Latin1Character>(m_text.subspan(range.offset, range.length)));
}

unsigned ImageText::hash(StringID string) const
{
    const ImageString& record = m_strings[string.index()];
    if (record.hasIdentifierNumber())
        return m_identifiers[record.hashOrIdentifierNumber()].hashOrSymbol();
    return record.hashOrIdentifierNumber();
}

bool ImageText::equals(StringID string, StringView text) const
{
    return hasText(string) && view(string) == text;
}

String ImageText::createString(StringID string) const
{
    StringView text = view(string);
    if (text.isEmpty())
        return emptyString();
    if (text.is8Bit())
        return StringImpl::createWithoutCopyingWithHash(text.span8(), hash(string));
    return StringImpl::createWithoutCopyingWithHash(text.span16(), hash(string));
}

String ImageText::createAtom(StringID string) const
{
    StringView text = view(string);
    if (text.is8Bit())
        return createAtomWithoutCopying(text.span8(), hash(string));
    return createAtomWithoutCopying(text.span16(), hash(string));
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
