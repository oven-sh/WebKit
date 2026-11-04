/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "JSString.h"
#include "PyBytecode.h"
#include "PyObject.h"
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

unsigned strLength(JSGlobalObject*, JSString*);
// Negative, 0 or positive. By code point, which is not the order of UTF-16 code units.
int compareStrings(StringView, StringView);
// One of the six rich comparisons.
JSValue strCompare(JSGlobalObject*, JSString*, JSString*, CompareOperator);
void appendCodePoint(StringBuilder&, char32_t);
// The count may be anything a program asked for.
void appendRepeated(StringBuilder&, char32_t, int64_t count);
String stringFromCodePoint(char32_t);

// repr(str)
// (These two: a null String if the result is too long.)
String strRepr(StringView);
// With everything beyond ASCII escaped.
String asciiEscape(StringView);

// UTF-8, with the handler "surrogatepass" if `allowSurrogates`; otherwise nullopt if there is a lone surrogate.
std::optional<Vector<uint8_t>> tryEncodeUTF8(StringView, bool allowSurrogates = false);
// Never fails: what is not UTF-8 becomes U+FFFD... for messages only.
String decodeUTF8Lenient(std::span<const uint8_t>);
// As os.fsdecode() and os.fsencode(): UTF-8 with "surrogateescape".
String decodeFilesystem(std::span<const uint8_t>);
Vector<uint8_t> encodeFilesystem(StringView);

} // namespace Python
} // namespace JSC
