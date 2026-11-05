/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "JSString.h"
#include "PyBytecode.h"
#include "PyObject.h"
#include <wtf/MallocSpan.h>
#include <wtf/text/StringBuilder.h>

namespace JSC {
namespace Python {

// A str is a JSString. Python counts code points where JS counts UTF-16 code units, so a surrogate pair is one character;
// a surrogate on its own is one too.

// An instance of a class derived from str.
class PyStr final : public PyObject {
public:
    PY_DECLARE_CELL(PyStr, PyObject, PyStrType);
    DECLARE_VISIT_CHILDREN;

    static PyStr* create(VM&, Structure*, PyType*, JSString*);
    JSString* value() const { return m_value.get(); }

private:
    PyStr(VM&, Structure*, PyType*, JSString*);

    WriteBarrier<JSString> m_value;
};

ALWAYS_INLINE bool isExactStr(JSValue value) { return value && value.isString(); }
ALWAYS_INLINE bool isStr(JSValue value) { return value && (value.isString() || PyStr::is(value)); }
ALWAYS_INLINE JSString* strValue(JSValue value)
{
    ASSERT(isStr(value));
    return value.isString() ? asString(value) : uncheckedDowncast<PyStr>(value.asCell())->value();
}
ALWAYS_INLINE JSString* tryStrValue(JSValue value) { return isStr(value) ? strValue(value) : nullptr; }

// The text of a string that is not a rope.
inline String stringOf(JSString* string)
{
    ASSERT(!string->isRope());
    return string->tryGetValue();
}

// The same text, as a string that is the one of its kind: what names are compared by.
JSString* internString(JSGlobalObject*, JSString*);
JSString* internString(VM&, const String&);
JSString* internString(VM&, ASCIILiteral);
// For who has the text's StringHasher::computeLiteralHashAndMaskTop8Bits() from when Bun was built.
JSString* internString(VM&, ASCIILiteral, unsigned hash);
JSString* makeStr(VM&, const String&);
JSString* makeStr(VM&, String&&);
JSString* makeStr(VM&, ASCIILiteral);
// string[from : from + length], in code units. The part has the text of the whole for its own only if it is long and at least half of it, so
// that what a program keeps of a text keeps at most twice itself alive. (jsSubstring() shares whenever that takes less room than a copy:
// one word of a file then keeps the file.) A program that eats a text from the front, `s = s[n:]`, copies what is left each time it
// has eaten half. A copy has one byte to the character if that will do.
JSString* partOfStr(VM&, const String&, unsigned from, unsigned length);

// Text that is put together piece by piece. What a program asks for may be too long to be a str: the builder then takes no more,
// and makeStr() raises a MemoryError. (A StringBuilder made the way WTF makes them by default ends the process.)
class StrBuilder : public StringBuilder {
public:
    StrBuilder()
        : StringBuilder(OverflowPolicy::RecordOverflow)
    {
    }
    // Null: it is too long.
    String toStringOrNull() { return hasOverflowed() ? String() : toString(); }
};
// Null, with a MemoryError: it is too long.
JSString* makeStr(JSGlobalObject*, StrBuilder&);
// For what toStringOrNull() returned.
JSString* makeStrUnlessTooLong(JSGlobalObject*, String&&);
inline JSString* makeStr(JSGlobalObject* globalObject, const String& string) { return makeStr(getVM(globalObject), string); }
inline JSString* makeStr(JSGlobalObject* globalObject, String&& string) { return makeStr(getVM(globalObject), WTF::move(string)); }

// For a str that has surrogate pairs: where, in code units, every 64th code point is.
class StrCheckpoints : public RefCounted<StrCheckpoints> {
public:
    static constexpr unsigned interval = 64;
    static Ref<StrCheckpoints> create() { return adoptRef(*new StrCheckpoints); }
    Vector<unsigned> offsets;
};

// The text of a str, with what it takes to index it by code point.
//
// Whether text in 16-bit units has surrogate pairs, and where, is found by going through it. A program that goes through a text
// character by character, text[i] after text[i], would do that once for each. So it is remembered for the texts last asked about
// (StrIndexCache, of the Runtime): the constructors that are given a global object do.
//
// The text of a JSString is borrowed, not retained: counting references to a StringImpl is atomic, and two such operations cost more
// than most methods of str do. It stays put as for a GCOwnedDataScope, which this holds: see JSString::swapToAtomString().
class StrView {
    WTF_FORBID_HEAP_ALLOCATION;
public:
    StrView() = default;
    // Check for an exception afterwards: flattening a rope may run out of memory.
    ALWAYS_INLINE StrView(JSGlobalObject* globalObject, JSString* string)
        : m_text(string, string->isRope() ? flatten(globalObject, string) : string->tryGetValueImpl())
    {
        initialize(globalObject);
    }
    StrView(JSGlobalObject* globalObject, const String& string)
        : m_text(nullptr, string.impl())
        , m_retained(string)
    {
        initialize(globalObject);
    }
    explicit StrView(const String&);

    // As JSString::valueInternal(): a String is a pointer to a StringImpl and nothing else.
    const String& string() const { return *std::bit_cast<const String*>(&m_text.data); }
    StringView view() const { return string(); }
    bool is8Bit() const { return string().is8Bit(); }
    // In code units.
    unsigned unitLength() const { return string().length(); }
    // Every character is one code unit.
    bool isSimple() const { return m_isSimple; }
    // In code points.
    unsigned length() const { return m_length; }
    char32_t at(unsigned index) const { return m_isSimple ? string()[index] : atSlow(index); }
    // From an index in code points to one in code units, and back.
    unsigned unitOffset(unsigned index) const { return m_isSimple ? index : unitOffsetSlow(index); }
    unsigned indexOfUnitOffset(unsigned offset) const { return m_isSimple ? offset : indexOfUnitOffsetSlow(offset); }
    String substring(unsigned start, unsigned end) const;

private:
    static StringImpl* flatten(JSGlobalObject*, JSString*);
    ALWAYS_INLINE void initialize(JSGlobalObject* globalObject)
    {
        m_length = string().length();
        if (!string().is8Bit()) [[unlikely]]
            lookThroughOrRecall(globalObject);
    }
    void lookThrough();
    void lookThroughOrRecall(JSGlobalObject*);
    char32_t atSlow(unsigned index) const;
    unsigned unitOffsetSlow(unsigned index) const;
    unsigned indexOfUnitOffsetSlow(unsigned offset) const;

    GCOwnedDataScope<StringImpl*> m_text;
    String m_retained; // The text, when it is not that of a JSString.
    unsigned m_length { 0 };
    bool m_isSimple { true };
    RefPtr<StrCheckpoints> m_checkpoints; // Only if it is long and not simple.
};
static_assert(sizeof(String) == sizeof(StringImpl*));

// The characters of a text that has surrogate pairs, each in a place of its own: for code that goes to and fro in a text by index.
// (Not a Vector, which holds 2**31 bytes at most and ends the process beyond.)
class WideText {
    WTF_MAKE_NONCOPYABLE(WideText);
public:
    WideText() = default;
    // NoPairs: every unit is a character, and nothing is kept. NoRoom: there is no memory for them.
    enum class Result : uint8_t { NoPairs, Done, NoRoom };
    Result set(std::span<const char16_t>);
    size_t size() const { return m_characters.size(); }
    ALWAYS_INLINE char32_t operator[](size_t index) const { return m_characters[index]; }
    std::span<const char32_t> span() const LIFETIME_BOUND { return m_characters; }

private:
    MallocSpan<char32_t> m_memory; // Room for as many characters as there are units.
    std::span<const char32_t> m_characters;
};

unsigned strLength(JSGlobalObject*, JSString*);
// Negative, 0 or positive. By code point, which is not the order of UTF-16 code units.
int compareStrings(StringView, StringView);
// One of the six rich comparisons.
JSValue strCompare(JSGlobalObject*, JSString*, JSString*, CompareOperator);
void appendCodePoint(StringBuilder&, char32_t);
// The count may be anything a program asked for.
void appendRepeated(StringBuilder&, char32_t, int64_t count);

// For text that is mostly short and Latin-1: it is put together on the stack and copied once, into a string of just its size. (A
// StringBuilder allocates a buffer, and a second time when the text is taken out of it.) What does not fit goes on in a StrBuilder.
// The room is not written to before it is used: not to be held across code of the program where it can be helped.
class ShortStrBuilder {
    WTF_FORBID_HEAP_ALLOCATION;
public:
    bool isEmpty() const { return m_isLong ? !m_builder.hasOverflowed() && m_builder.isEmpty() : !m_length; }

    ALWAYS_INLINE void append(std::span<const Latin1Character> text)
    {
        if (m_isLong || text.size() > sizeof(m_short) - m_length) [[unlikely]]
            return builder().append(text);
        memcpySpan(std::span { m_short }.subspan(m_length), text);
        m_length += text.size();
    }
    ALWAYS_INLINE void append(StringView text)
    {
        if (text.is8Bit()) [[likely]]
            return append(text.span8());
        builder().append(text);
    }
    void append(char character) { append(std::span { byteCast<Latin1Character>(&character), 1 }); }
    void append(int64_t number)
    {
        std::array<Latin1Character, 20> digits;
        unsigned length = lengthOfIntegerAsString(number);
        writeIntegerToBuffer(number, std::span { digits }.first(length));
        append(std::span<const Latin1Character> { digits }.first(length));
    }

    void appendRepeated(char character, int64_t count)
    {
        if (count <= 0)
            return;
        if (m_isLong || count > static_cast<int64_t>(sizeof(m_short) - m_length)) [[unlikely]]
            return Python::appendRepeated(builder(), character, count);
        memsetSpan(std::span { m_short }.subspan(m_length, count), static_cast<Latin1Character>(character));
        m_length += count;
    }
    bool hasOverflowed() const { return m_isLong && m_builder.hasOverflowed(); }
    bool endsWith(char character) const
    {
        if (!m_isLong)
            return m_length && m_short[m_length - 1] == character;
        return !m_builder.hasOverflowed() && !m_builder.isEmpty() && m_builder[m_builder.length() - 1] == character;
    }
    void removeLast()
    {
        if (!m_isLong)
            --m_length;
        else if (!m_builder.hasOverflowed())
            m_builder.shrink(m_builder.length() - 1);
    }

    // For everything else there is to append.
    StrBuilder& builder()
    {
        if (!m_isLong) {
            m_isLong = true;
            m_builder.append(std::span<const Latin1Character> { m_short }.first(m_length));
        }
        return m_builder;
    }

    // Null: it is too long.
    String toStringOrNull() { return m_isLong ? m_builder.toStringOrNull() : String(std::span<const Latin1Character> { m_short }.first(m_length)); }

private:
    std::array<Latin1Character, 232> m_short;
    unsigned m_length { 0 };
    bool m_isLong { false };
    StrBuilder m_builder;
};
String stringFromCodePoint(char32_t);

// repr(str)
// (These two: a null String if the result is too long.)
String strRepr(StringView);
// With everything beyond ASCII escaped.
String asciiEscape(StringView);

// UTF-8, with the handler "surrogatepass" if `allowSurrogates`; otherwise nullopt if there is a lone surrogate. Only for text that is known
// to be short: a Vector ends the process where it cannot hold the bytes. For what a program hands over there is encodeUTF8(), or
// appendUTF8() in PyBytes.h.
std::optional<Vector<uint8_t>> tryEncodeUTF8(StringView, bool allowSurrogates = false);
// False: an exception, that of str.encode() for a lone surrogate, or a MemoryError for more than 2**30 bytes.
bool encodeUTF8(JSGlobalObject*, JSString*, Vector<uint8_t>& result);
// The same with a NUL behind it. False also for a NUL in the text: ValueError, "embedded null character".
bool encodeCString(JSGlobalObject*, JSString*, Vector<uint8_t>& result);
// How many bytes the text takes (nothing: it has halves of surrogate pairs on their own, which have no UTF-8), and the bytes, into room
// of just that size.
std::optional<size_t> lengthInUTF8(StringView);
void writeUTF8(StringView, std::span<uint8_t>);
// Never fails: what is not UTF-8 becomes U+FFFD... for messages only.
String decodeUTF8Lenient(std::span<const uint8_t>);
// As os.fsdecode() and os.fsencode(): UTF-8 with "surrogateescape".
String decodeFilesystem(std::span<const uint8_t>);
Vector<uint8_t> encodeFilesystem(StringView);

} // namespace Python
} // namespace JSC
