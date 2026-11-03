/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "JSString.h"
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
JSString* makeStr(VM&, const String&);
JSString* makeStr(VM&, ASCIILiteral);

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
JSString* makeStrUnlessTooLong(JSGlobalObject*, const String&);
inline JSString* makeStr(JSGlobalObject* globalObject, const String& string) { return makeStr(getVM(globalObject), string); }

// The text of a str, with what it takes to index it by code point.
class StrView {
public:
    StrView() = default;
    // Check for an exception afterwards: flattening a rope may run out of memory.
    StrView(JSGlobalObject*, JSString*);
    explicit StrView(const String&);

    const String& string() const { return m_string; }
    StringView view() const { return m_string; }
    bool is8Bit() const { return m_string.is8Bit(); }
    // In code units.
    unsigned unitLength() const { return m_string.length(); }
    // Every character is one code unit.
    bool isSimple() const { return m_isSimple; }
    // In code points.
    unsigned length() const { return m_length; }
    char32_t at(unsigned index) const;
    // From an index in code points to one in code units, and back.
    unsigned unitOffset(unsigned index) const;
    unsigned indexOfUnitOffset(unsigned offset) const;
    String substring(unsigned start, unsigned end) const;

private:
    String m_string;
    unsigned m_length { 0 };
    bool m_isSimple { true };
};

unsigned strLength(JSGlobalObject*, JSString*);
void appendCodePoint(StringBuilder&, char32_t);
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
