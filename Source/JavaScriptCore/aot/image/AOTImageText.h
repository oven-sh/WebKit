/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTImageForward.h"
#include "JSExportMacros.h"
#include <optional>
#include <span>
#include <wtf/text/StringView.h>
#include <wtf/text/WTFString.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

struct ImageString {
    static constexpr unsigned lengthBits = 8;
    static constexpr uint32_t lengthMask = (1u << lengthBits) - 1;
    static constexpr uint32_t hashMask = (1u << 24) - 1;
    static constexpr uint32_t is16BitFlag = 1u << 24;
    static constexpr uint32_t isAtomFlag = 1u << 25;
    static constexpr uint32_t hasIdentifierNumberFlag = 1u << 26;

    bool hasText() const { return !!location; }
    bool is16Bit() const { return hashAndFlags & is16BitFlag; }
    bool isAtom() const { return hashAndFlags & isAtomFlag; }
    bool hasIdentifierNumber() const { return hashAndFlags & hasIdentifierNumberFlag; }
    uint32_t hashOrIdentifierNumber() const { return hashAndFlags & hashMask; }

    uint32_t location;
    uint32_t hashAndFlags;
};
static_assert(sizeof(ImageString) == 8 && isImageRecord<ImageString>);

struct ImageLongString {
    uint32_t offset;
    uint32_t length;
};
static_assert(sizeof(ImageLongString) == 8 && isImageRecord<ImageLongString>);

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

    uint32_t textPlusOne;
    uint32_t bits;
};
static_assert(sizeof(ImageIdentifier) == 8 && isImageRecord<ImageIdentifier>);

class ImageText {
public:
    JS_EXPORT_PRIVATE explicit ImageText(const Image&);

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

    std::optional<uint32_t> identifierNumber(StringID) const;
    const ImageIdentifier& identifier(uint32_t number) const { return m_identifiers[number]; }
    StringID stringConstant(uint32_t number) const { return StringID(m_stringConstants[number]); }

private:
    std::span<const ImageString> m_strings;
    std::span<const ImageLongString> m_longStrings;
    std::span<const uint8_t> m_text;
    std::span<const ImageIdentifier> m_identifiers;
    std::span<const uint32_t> m_stringConstants;
};

inline std::optional<uint32_t> ImageText::identifierNumber(StringID string) const
{
    const ImageString& record = m_strings[string.index()];
    if (!record.hasIdentifierNumber())
        return std::nullopt;
    return record.hashOrIdentifierNumber();
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
