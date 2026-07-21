/*
 *  Copyright (C) 2003-2019 Apple Inc. All rights reserved.
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Library General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Library General Public License for more details.
 *
 *  You should have received a copy of the GNU Library General Public License
 *  along with this library; see the file COPYING.LIB.  If not, write to
 *  the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 *  Boston, MA 02110-1301, USA.
 *
 */

#pragma once

#include "ArrayConventions.h"
#include "PrivateName.h"
#include "SmallStrings.h"
#include <wtf/text/CString.h>
#include <wtf/text/ParsingUtilities.h>
#include <wtf/text/UniquedStringImpl.h>
#include <wtf/text/WTFString.h>

#if USE(BUN_JSC_ADDITIONS)
#include "InlinePropertyKey.h"
#endif

namespace JSC {

class CallFrame;

ALWAYS_INLINE bool isIndex(uint32_t index)
{
    return index <= MAX_ARRAY_INDEX;
}

template <typename CharType>
ALWAYS_INLINE std::optional<uint32_t> parseIndex(std::span<const CharType> characters)
{
    // An empty string is not a number.
    if (characters.empty())
        return std::nullopt;

    // Get the first character, turning it into a digit.
    uint32_t value = characters.front() - '0';
    if (value > 9)
        return std::nullopt;

    // Check for leading zeros. If the first characher is 0, then the
    // length of the string must be one - e.g. "042" is not equal to "42".
    if (!value && characters.size() > 1)
        return std::nullopt;

    skip(characters, 1);
    while (!characters.empty()) {
        // Multiply value by 10, checking for overflow out of 32 bits.
        if (value > 0xFFFFFFFFU / 10)
            return std::nullopt;
        value *= 10;

        // Get the next character, turning it into a digit.
        uint32_t newValue = characters.front() - '0';
        if (newValue > 9)
            return std::nullopt;

        // Add in the old value, checking for overflow out of 32 bits.
        newValue += value;
        if (newValue < value)
            return std::nullopt;
        value = newValue;
        skip(characters, 1);
    }

    if (!isIndex(value))
        return std::nullopt;
    return value;
}

ALWAYS_INLINE std::optional<uint32_t> parseIndex(const StringImpl& impl)
{
    return impl.is8Bit() ? parseIndex(impl.span8()) : parseIndex(impl.span16());
}

#if USE(BUN_JSC_ADDITIONS)
class Identifier {
    friend class Structure;
public:
    Identifier() = default;
    enum class EmptyIdentifierFlag { EmptyIdentifier };
    Identifier(EmptyIdentifierFlag)
    {
        auto* empty = StringImpl::empty();
        empty->ref();
        m_bits = reinterpret_cast<uintptr_t>(empty);
        ASSERT(empty->isAtom());
    }

    // m_materializedString is a lazy cache for string(); never copy/move it so
    // Identifier copy/move/assign stay single-word and skip the extra AtomString
    // ref/deref. The cache re-populates on first string() per instance.
    Identifier(const Identifier& other)
        : m_bits(other.m_bits)
    {
        if (m_bits)
            uidRef(reinterpret_cast<UniquedStringImpl*>(m_bits));
    }

    Identifier(Identifier&& other)
        : m_bits(std::exchange(other.m_bits, 0))
    {
    }

    ~Identifier()
    {
        if (m_bits)
            uidDeref(reinterpret_cast<UniquedStringImpl*>(m_bits));
    }

    Identifier& operator=(const Identifier& other)
    {
        uintptr_t newBits = other.m_bits;
        if (newBits)
            uidRef(reinterpret_cast<UniquedStringImpl*>(newBits));
        uintptr_t oldBits = std::exchange(m_bits, newBits);
        if (oldBits)
            uidDeref(reinterpret_cast<UniquedStringImpl*>(oldBits));
        // m_bits changed → cache is stale. Common case was already null so this
        // is a single predictable branch inside AtomString::operator=.
        m_materializedString = AtomString();
        return *this;
    }

    Identifier& operator=(Identifier&& other)
    {
        uintptr_t oldBits = std::exchange(m_bits, std::exchange(other.m_bits, 0));
        if (oldBits)
            uidDeref(reinterpret_cast<UniquedStringImpl*>(oldBits));
        m_materializedString = AtomString();
        return *this;
    }

    enum class FromFiberWordTag { T };
    static Identifier fromFiberWord(uintptr_t word) { return Identifier(FromFiberWordTag::T, word); }

    const AtomString& string() const LIFETIME_BOUND
    {
        if (!m_bits)
            return m_materializedString;
        if (m_materializedString.isNull()) {
            if (isInlinePropertyKey(m_bits)) {
                unsigned len = inlinePropertyKeyLength(m_bits);
                const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&m_bits);
                if (inlinePropertyKeyIs8Bit(m_bits))
                    m_materializedString = AtomString(std::span<const Latin1Character> { bytes + 1, len });
                else
                    m_materializedString = AtomString(std::span<const char16_t> { reinterpret_cast<const char16_t*>(bytes + 2), len });
            } else
                m_materializedString = AtomString(reinterpret_cast<UniquedStringImpl*>(m_bits));
        }
        return m_materializedString;
    }

    // May return a fiber-word-tagged pointer; callers are phase-A shimmed.
    UniquedStringImpl* impl() const { return reinterpret_cast<UniquedStringImpl*>(m_bits); }

    RefPtr<AtomStringImpl> releaseImpl()
    {
        string();
        uintptr_t oldBits = std::exchange(m_bits, 0);
        if (oldBits)
            uidDeref(reinterpret_cast<UniquedStringImpl*>(oldBits));
        return m_materializedString.releaseImpl();
    }

    int length() const { return m_bits ? static_cast<int>(uidLength(reinterpret_cast<UniquedStringImpl*>(m_bits))) : 0; }

    // Bypass string() so a fiber-word decode never touches the thread-local
    // AtomStringTable: FTL compiler threads reach here via
    // CodeBlock::inferredName() -> ecmaName().utf8(), and an atom materialized
    // there would later be destroyed on the main thread's GC sweep.
    CString ascii() const { return stringWithoutAtomizing().ascii(); }
    CString utf8() const { return stringWithoutAtomizing().utf8(); }

    String stringWithoutAtomizing() const
    {
        if (!m_bits)
            return String();
        if (isInlinePropertyKey(m_bits)) {
            unsigned len = inlinePropertyKeyLength(m_bits);
            const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&m_bits);
            if (inlinePropertyKeyIs8Bit(m_bits))
                return String(std::span<const Latin1Character> { bytes + 1, len });
            return String(std::span<const char16_t> { reinterpret_cast<const char16_t*>(bytes + 2), len });
        }
        return String(reinterpret_cast<StringImpl*>(m_bits));
    }

    // There's 2 functions to construct Identifier from string, (1) fromString and (2) fromUid.
    // They have different meanings in keeping or discarding symbol-ness of strings.
    // (1): fromString
    // Just construct Identifier from string. String held by Identifier is always atomized.
    // Symbol-ness of StringImpl*, which represents that the string is inteded to be used for ES6 Symbols, is discarded.
    // So a constructed Identifier never represents a symbol.
    // (2): fromUid
    // `StringImpl* uid` represents ether String or Symbol property.
    // fromUid keeps symbol-ness of provided StringImpl* while fromString discards it.
    // Use fromUid when constructing Identifier from StringImpl* which may represent symbols.

    static Identifier fromString(VM&, ASCIILiteral);
    static Identifier fromString(VM&, std::span<const Latin1Character>);
    static Identifier fromString(VM&, std::span<const char16_t>);
    static Identifier fromString(VM&, const String&);
    static Identifier fromString(VM&, AtomStringImpl*);
    static Identifier fromString(VM&, Ref<AtomStringImpl>&&);
    static Identifier fromString(VM&, const AtomString&);
    static Identifier fromString(VM&, SymbolImpl*);

    static Identifier NODELETE fromUid(VM&, UniquedStringImpl* uid);
    static Identifier fromUid(const PrivateName&);
    static Identifier fromUid(SymbolImpl&);

    static inline Identifier createLatin1(VM& vm, std::span<const char16_t> string); // Defined in IdentifierInlines.h

    JS_EXPORT_PRIVATE static Identifier from(VM&, unsigned y);
    JS_EXPORT_PRIVATE static Identifier from(VM&, int y);
    JS_EXPORT_PRIVATE static Identifier from(VM&, double y);
    ALWAYS_INLINE static Identifier from(VM& vm, uint64_t y)
    {
        if (static_cast<uint32_t>(y) == y)
            return from(vm, static_cast<uint32_t>(y));
        ASSERT(static_cast<uint64_t>(static_cast<double>(y)) == y);
        return from(vm, static_cast<double>(y));
    }

    bool isNull() const { return !m_bits; }
    // Matches AtomString::isEmpty(): null OR zero-length. BreakNode/ContinueNode
    // pass a null-m_bits Identifier for the unlabeled case and test isEmpty().
    bool isEmpty() const { return !m_bits || (!isInlinePropertyKey(m_bits) && !reinterpret_cast<StringImpl*>(m_bits)->length()); }
    bool isSymbol() const { return m_bits && uidIsSymbol(reinterpret_cast<UniquedStringImpl*>(m_bits)); }
    bool isPrivateName() const { return isSymbol() && static_cast<const SymbolImpl*>(impl())->isPrivate(); }

    friend bool operator==(const Identifier&, const Identifier&);

    static bool equal(const StringImpl*, std::span<const Latin1Character>);
    static bool equal(const StringImpl*, std::span<const char16_t>);
    static bool equal(const StringImpl* a, const StringImpl* b) { return ::equal(a, b); }

    void dump(PrintStream&) const;

    // Phase D.4 coherence: every construction path for a 2..5-char Latin-1
    // name must yield the same encodeInline8 fiber word so PropertyTable's
    // pointer-identity compare hits regardless of which producer ran.
    static uintptr_t canonicalFiberWordFor(const StringImpl*); // Defined in IdentifierInlines.h

private:
    uintptr_t m_bits { 0 };
    mutable AtomString m_materializedString;

    Identifier(FromFiberWordTag, uintptr_t word)
        : m_bits(word)
    {
        ASSERT(isInlinePropertyKey(word));
    }

    inline Identifier(VM&, std::span<const Latin1Character>); // Defined in IdentifierInlines.h
    inline Identifier(VM&, std::span<const char16_t>); // Defined in IdentifierInlines.h
    ALWAYS_INLINE Identifier(VM&, ASCIILiteral); // Defined in IdentifierInlines.h
    inline Identifier(VM&, AtomStringImpl*); // Defined in IdentifierInlines.h
    inline Identifier(VM&, const AtomString&); // Defined in IdentifierInlines.h
    inline Identifier(VM&, const String&);
    inline Identifier(VM&, StringImpl*);
    inline Identifier(VM&, Ref<AtomStringImpl>&&); // Defined in IdentifierInlines.h

    Identifier(SymbolImpl& uid)
    {
        uid.ref();
        m_bits = reinterpret_cast<uintptr_t>(&uid);
    }

    static bool equal(const Identifier& a, const Identifier& b) { return a.m_bits == b.m_bits; }

    template <typename T> inline static Ref<AtomStringImpl> add(VM&, std::span<const T>); // Defined in IdentifierInlines.h
    static Ref<AtomStringImpl> add8(VM&, std::span<const char16_t>);
    template <typename T> ALWAYS_INLINE static constexpr bool canUseSingleCharacterString(T);

    static Ref<AtomStringImpl> add(VM&, StringImpl*);
    inline static Ref<AtomStringImpl> add(VM&, ASCIILiteral); // Defined in IdentifierInlines.h

#ifndef NDEBUG
    JS_EXPORT_PRIVATE static void checkCurrentAtomStringTable(VM&);
#else
    JS_EXPORT_PRIVATE NO_RETURN_DUE_TO_CRASH static void checkCurrentAtomStringTable(VM&);
#endif
};
#else
class Identifier {
    friend class Structure;
public:
    Identifier() = default;
    enum class EmptyIdentifierFlag { EmptyIdentifier };
    Identifier(EmptyIdentifierFlag) : m_string(StringImpl::empty()) { ASSERT(m_string.impl()->isAtom()); }

    const AtomString& string() const LIFETIME_BOUND { return m_string; }

    UniquedStringImpl* impl() const { return m_string.impl(); }
    RefPtr<AtomStringImpl> releaseImpl() { return m_string.releaseImpl(); }

    int length() const { return m_string.length(); }

    CString ascii() const { return m_string.string().ascii(); }
    CString utf8() const { return m_string.string().utf8(); }

    // There's 2 functions to construct Identifier from string, (1) fromString and (2) fromUid.
    // They have different meanings in keeping or discarding symbol-ness of strings.
    // (1): fromString
    // Just construct Identifier from string. String held by Identifier is always atomized.
    // Symbol-ness of StringImpl*, which represents that the string is inteded to be used for ES6 Symbols, is discarded.
    // So a constructed Identifier never represents a symbol.
    // (2): fromUid
    // `StringImpl* uid` represents ether String or Symbol property.
    // fromUid keeps symbol-ness of provided StringImpl* while fromString discards it.
    // Use fromUid when constructing Identifier from StringImpl* which may represent symbols.

    static Identifier fromString(VM&, ASCIILiteral);
    static Identifier fromString(VM&, std::span<const Latin1Character>);
    static Identifier fromString(VM&, std::span<const char16_t>);
    static Identifier fromString(VM&, const String&);
    static Identifier fromString(VM&, AtomStringImpl*);
    static Identifier fromString(VM&, Ref<AtomStringImpl>&&);
    static Identifier fromString(VM&, const AtomString&);
    static Identifier fromString(VM&, SymbolImpl*);

    static Identifier NODELETE fromUid(VM&, UniquedStringImpl* uid);
    static Identifier fromUid(const PrivateName&);
    static Identifier fromUid(SymbolImpl&);

    static inline Identifier createLatin1(VM& vm, std::span<const char16_t> string); // Defined in IdentifierInlines.h

    JS_EXPORT_PRIVATE static Identifier from(VM&, unsigned y);
    JS_EXPORT_PRIVATE static Identifier from(VM&, int y);
    JS_EXPORT_PRIVATE static Identifier from(VM&, double y);
    ALWAYS_INLINE static Identifier from(VM& vm, uint64_t y)
    {
        if (static_cast<uint32_t>(y) == y)
            return from(vm, static_cast<uint32_t>(y));
        ASSERT(static_cast<uint64_t>(static_cast<double>(y)) == y);
        return from(vm, static_cast<double>(y));
    }

    bool isNull() const { return m_string.isNull(); }
    bool isEmpty() const { return m_string.isEmpty(); }
    bool isSymbol() const { return !isNull() && impl()->isSymbol(); }
    bool isPrivateName() const { return isSymbol() && static_cast<const SymbolImpl*>(impl())->isPrivate(); }

    friend bool operator==(const Identifier&, const Identifier&);

    static bool equal(const StringImpl*, std::span<const Latin1Character>);
    static bool equal(const StringImpl*, std::span<const char16_t>);
    static bool equal(const StringImpl* a, const StringImpl* b) { return ::equal(a, b); }

    void dump(PrintStream&) const;

private:
    AtomString m_string;

    inline Identifier(VM&, std::span<const Latin1Character>); // Defined in IdentifierInlines.h
    inline Identifier(VM&, std::span<const char16_t>); // Defined in IdentifierInlines.h
    ALWAYS_INLINE Identifier(VM&, ASCIILiteral); // Defined in IdentifierInlines.h
    inline Identifier(VM&, AtomStringImpl*); // Defined in IdentifierInlines.h
    inline Identifier(VM&, const AtomString&); // Defined in IdentifierInlines.h
    inline Identifier(VM&, const String&);
    inline Identifier(VM&, StringImpl*);

    Identifier(VM&, Ref<AtomStringImpl>&& impl)
        : m_string(WTF::move(impl))
    { }

    Identifier(SymbolImpl& uid)
        : m_string(&uid)
    { }

    static bool equal(const Identifier& a, const Identifier& b) { return a.m_string.impl() == b.m_string.impl(); }

    template <typename T> inline static Ref<AtomStringImpl> add(VM&, std::span<const T>); // Defined in IdentifierInlines.h
    static Ref<AtomStringImpl> add8(VM&, std::span<const char16_t>);
    template <typename T> ALWAYS_INLINE static constexpr bool canUseSingleCharacterString(T);

    static Ref<AtomStringImpl> add(VM&, StringImpl*);
    inline static Ref<AtomStringImpl> add(VM&, ASCIILiteral); // Defined in IdentifierInlines.h

#ifndef NDEBUG
    JS_EXPORT_PRIVATE static void checkCurrentAtomStringTable(VM&);
#else
    JS_EXPORT_PRIVATE NO_RETURN_DUE_TO_CRASH static void checkCurrentAtomStringTable(VM&);
#endif
};
#endif

template <> ALWAYS_INLINE constexpr bool Identifier::canUseSingleCharacterString(Latin1Character)
{
    static_assert(maxSingleCharacterString == 0xff);
    return true;
}

template <> ALWAYS_INLINE constexpr bool Identifier::canUseSingleCharacterString(char16_t c)
{
    return (c <= maxSingleCharacterString);
}

inline bool operator==(const Identifier& a, const Identifier& b)
{
    return Identifier::equal(a, b);
}

inline bool Identifier::equal(const StringImpl* r, std::span<const Latin1Character> s)
{
#if USE(BUN_JSC_ADDITIONS)
    if (isInlinePropertyKey(reinterpret_cast<const UniquedStringImpl*>(r))) [[unlikely]] {
        uintptr_t w = reinterpret_cast<uintptr_t>(r);
        if (!inlinePropertyKeyIs8Bit(w))
            return false;
        auto span = inlinePropertyKeySpan8(w);
        return span.size() == s.size() && !memcmp(span.data(), s.data(), s.size());
    }
#endif
    return WTF::equal(r, s);
}

inline bool Identifier::equal(const StringImpl* r, std::span<const char16_t> s)
{
#if USE(BUN_JSC_ADDITIONS)
    if (isInlinePropertyKey(reinterpret_cast<const UniquedStringImpl*>(r))) [[unlikely]] {
        uintptr_t w = reinterpret_cast<uintptr_t>(r);
        if (!inlinePropertyKeyIs8Bit(w))
            return false;
        auto span = inlinePropertyKeySpan8(w);
        if (span.size() != s.size())
            return false;
        for (size_t i = 0; i < s.size(); ++i)
            if (span[i] != s[i])
                return false;
        return true;
    }
#endif
    return WTF::equal(r, s);
}

ALWAYS_INLINE std::optional<uint32_t> parseIndex(const Identifier& identifier)
{
#if USE(BUN_JSC_ADDITIONS)
    auto uid = identifier.impl();
    if (!uid || uidIsSymbol(uid))
        return std::nullopt;
    if (isInlinePropertyKey(uid)) {
        // D.4 coherence means numeric strings "42".."9999999" arrive here as
        // fiber words; decode so obj["42"] still hits the indexed-property path.
        uintptr_t w = reinterpret_cast<uintptr_t>(uid);
        if (inlinePropertyKeyIs8Bit(w))
            return parseIndex(inlinePropertyKeySpan8(w));
        return std::nullopt;
    }
    return parseIndex(*uid);
#else
    auto uid = identifier.impl();
    if (!uid)
        return std::nullopt;
    if (uid->isSymbol())
        return std::nullopt;
    return parseIndex(*uid);
#endif
}

JSValue identifierToJSValue(VM&, const Identifier&);
// This will stringify private symbols. When leaking JSValues to
// non-internal code, make sure to use this function and not the above one.
JSValue identifierToSafePublicJSValue(VM&, const Identifier&);

// FIXME: It may be better for this to just be a typedef for PtrHash, since PtrHash may be cheaper to
// compute than loading the StringImpl's hash from memory. That change would also reduce the likelihood of
// crashes in code that somehow dangled a StringImpl.
// https://bugs.webkit.org/show_bug.cgi?id=150137
struct IdentifierRepHash : PtrHash<RefPtr<UniquedStringImpl>> {
    static unsigned hash(const UniquedStringImpl* key)
    {
#if USE(BUN_JSC_ADDITIONS)
        return uidHash(key);
#else
        return key->existingSymbolAwareHash();
#endif
    }
    static constexpr bool hasHashInValue = true;
};

struct IdentifierMapIndexHashTraits : HashTraits<int> {
    static int emptyValue() { return std::numeric_limits<int>::max(); }
    static constexpr bool emptyValueIsZero = false;
};

#if USE(BUN_JSC_ADDITIONS)
typedef UncheckedKeyHashSet<FiberAwareRefPtr, IdentifierRepHash> IdentifierSet;
typedef UncheckedKeyHashMap<FiberAwareRefPtr, int, IdentifierRepHash, HashTraits<FiberAwareRefPtr>, IdentifierMapIndexHashTraits> IdentifierMap;
typedef UncheckedKeyHashMap<UniquedStringImpl*, int, IdentifierRepHash, HashTraits<UniquedStringImpl*>, IdentifierMapIndexHashTraits> BorrowedIdentifierMap;
#else
typedef UncheckedKeyHashSet<RefPtr<UniquedStringImpl>, IdentifierRepHash> IdentifierSet;
typedef UncheckedKeyHashMap<RefPtr<UniquedStringImpl>, int, IdentifierRepHash, HashTraits<RefPtr<UniquedStringImpl>>, IdentifierMapIndexHashTraits> IdentifierMap;
typedef UncheckedKeyHashMap<UniquedStringImpl*, int, IdentifierRepHash, HashTraits<UniquedStringImpl*>, IdentifierMapIndexHashTraits> BorrowedIdentifierMap;
#endif

} // namespace JSC

namespace WTF {

#if USE(BUN_JSC_ADDITIONS)
// m_bits equality defines Identifier equality, but m_materializedString is a
// lazily-populated cache — two equal Identifiers can differ byte-wise there.
template <> struct VectorTraits<JSC::Identifier> : SimpleClassVectorTraits {
    static constexpr bool canCompareWithMemcmp = false;
};
#else
template <> struct VectorTraits<JSC::Identifier> : SimpleClassVectorTraits { };
#endif

} // namespace WTF
