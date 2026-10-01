/*
 * Copyright (C) 2026 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include "JSCJSValue.h"
#include "PageCount.h"
#include <unicode/utf16.h>
#include <wtf/FastMalloc.h>
#include <wtf/Vector.h>
#include <wtf/text/StringBuilder.h>
#include <wtf/text/WTFString.h>

namespace JSC {

class JSGlobalObject;

namespace Python {

class Buffer;

// The bytes of something that is being made, which come to as many as a program makes them. It can hold as many as a typed array can, which is more than a Vector can, and where a Vector brings everything down
// when there is no room for more, this remembers, as TextBuilder does, and takes no more. What makes a bytes or a bytearray of it raises MemoryError then, and anything else that is to have what is in it asks
// hasOverflowed() first.
class ByteVector final {
    WTF_MAKE_NONCOPYABLE(ByteVector);
public:
    // Explicit so that `{ }`, where bytes are wanted, is a span of none and not one of these.
    explicit ByteVector() = default;
    ByteVector(ByteVector&& other) { take(other); }
    ByteVector& operator=(ByteVector&& other)
    {
        if (this != &other) {
            release();
            take(other);
        }
        return *this;
    }
    ~ByteVector() { release(); }

    bool hasOverflowed() const { return m_hasOverflowed; }
    size_t size() const { return m_size; }
    bool isEmpty() const { return !m_size; }

    // Not all of what was put in it is there if there was no room, so it is not to be had.
    std::span<const uint8_t> span() const
    {
        RELEASE_ASSERT(!m_hasOverflowed);
        return { m_data, m_size };
    }
    std::span<uint8_t> mutableSpan()
    {
        RELEASE_ASSERT(!m_hasOverflowed);
        return { m_data, m_size };
    }
    uint8_t& operator[](size_t index) { return mutableSpan()[index]; }
    uint8_t operator[](size_t index) const { return span()[index]; }
    uint8_t last() const { return span().back(); }
    auto begin() const { return span().begin(); }
    auto end() const { return span().end(); }

    // False if there is no room for so many in all. It is not remembered: what asked can raise what it likes.
    bool tryReserveCapacity(size_t capacity)
    {
        if (capacity <= m_capacity)
            return true;
        if (capacity > MAX_ARRAY_BUFFER_SIZE)
            return false;
        uint8_t* data;
        if (isInline()) {
            if (!tryFastMalloc(capacity).getValue(data))
                return false;
            memcpy(data, m_inline, m_size);
        } else if (!WTF::tryFastRealloc(m_data, capacity).getValue(data))
            return false;
        m_data = data;
        m_capacity = capacity;
        return true;
    }

    void append(uint8_t byte)
    {
        if (m_size == m_capacity && !makeRoomFor(1)) [[unlikely]]
            return;
        m_data[m_size++] = byte;
    }
    void append(std::span<const uint8_t> bytes)
    {
        if (bytes.empty() || !makeRoomFor(bytes.size()))
            return;
        memcpy(m_data + m_size, bytes.data(), bytes.size());
        m_size += bytes.size();
    }
    void appendList(std::initializer_list<uint8_t> bytes) { append(std::span<const uint8_t> { bytes.begin(), bytes.size() }); }
    // So many of the one byte.
    void appendFill(uint8_t byte, size_t count)
    {
        if (!count || !makeRoomFor(count))
            return;
        memset(m_data + m_size, byte, count);
        m_size += count;
    }
    void removeLast()
    {
        RELEASE_ASSERT(m_size);
        --m_size;
    }
    void reverse() { std::ranges::reverse(mutableSpan()); }

private:
    bool isInline() const { return m_data == m_inline; }

    bool makeRoomFor(size_t more)
    {
        if (m_hasOverflowed)
            return false;
        if (more <= m_capacity - m_size)
            return true;
        // Twice as much, so that adding one at a time does not copy them all each time, or failing that only what is needed.
        if (more <= MAX_ARRAY_BUFFER_SIZE - m_size) {
            size_t needed = m_size + more;
            if (tryReserveCapacity(std::min<size_t>(std::max(needed, m_capacity * 2), MAX_ARRAY_BUFFER_SIZE)) || tryReserveCapacity(needed))
                return true;
        }
        m_hasOverflowed = true;
        return false;
    }

    void release()
    {
        if (!isInline())
            fastFree(m_data);
    }

    // What `other` has, leaving it empty. There is nothing here to let go of.
    void take(ByteVector& other)
    {
        m_size = other.m_size;
        m_hasOverflowed = other.m_hasOverflowed;
        if (other.isInline()) {
            memcpy(m_inline, other.m_inline, other.m_size);
            m_data = m_inline;
            m_capacity = sizeof(m_inline);
        } else {
            m_data = other.m_data;
            m_capacity = other.m_capacity;
        }
        other.m_data = other.m_inline;
        other.m_capacity = sizeof(m_inline);
        other.m_size = 0;
        other.m_hasOverflowed = false;
    }

    uint8_t m_inline[64]; // Most of what is made is short.
    uint8_t* m_data { m_inline };
    size_t m_size { 0 };
    size_t m_capacity { sizeof(m_inline) };
    bool m_hasOverflowed { false };
};

// ---- str.encode() and bytes.decode()

// PyUnicode_AsEncodedString() and PyUnicode_Decode(). A null String for `encoding` or `errors` is the default: "utf-8", and "strict". Nothing, or a null String, if they raised.
std::optional<ByteVector> encodeString(JSGlobalObject*, JSValue string, const String& encoding, const String& errors);
String decodeBytes(JSGlobalObject*, std::span<const uint8_t>, const String& encoding, const String& errors);
// The same, giving the object, which is the very one that the codec gave if it was one that a program registered. Empty if they raised.
JSValue encodeStringToObject(JSGlobalObject*, JSValue string, const String& encoding, const String& errors);
JSValue decodeBytesToObject(JSGlobalObject*, std::span<const uint8_t>, const String& encoding, const String& errors);

// ---- What the codecs are written with

// The characters of a str, by number. It is only if there are surrogate pairs in it that anything is made.
class CodePoints {
    WTF_MAKE_NONCOPYABLE(CodePoints);
public:
    explicit CodePoints(const String&);

    size_t size() const { return m_size; }
    char32_t operator[](size_t index) const
    {
        ASSERT(index < m_size);
        return m_characters8 ? m_characters8[index] : m_characters16 ? m_characters16[index] : m_expanded[index];
    }
    // PyUnicode_KIND() == PyUnicode_1BYTE_KIND, and PyUnicode_IS_ASCII(). It is what is in it that counts, and not how it is kept.
    bool isLatin1() const { return m_isLatin1; }
    bool isASCII() const { return m_isASCII; }
    // Calls the function with all of them, as a span of whatever they are kept as, for what goes through a great many and is not to ask which each time.
    template<typename Function>
    decltype(auto) withSpan(const Function& function) const
    {
        if (m_characters8)
            return function(std::span<const Latin1Character>(m_characters8, m_size));
        if (m_characters16)
            return function(std::span<const char16_t>(m_characters16, m_size));
        return function(m_expanded.span());
    }

private:
    String m_string;
    const Latin1Character* m_characters8 { nullptr };
    const char16_t* m_characters16 { nullptr };
    Vector<char32_t> m_expanded;
    size_t m_size { 0 };
    bool m_isLatin1 { true };
    bool m_isASCII { true };
};

// What a decoder writes to: _PyUnicodeWriter.
class TextWriter {
    WTF_MAKE_NONCOPYABLE(TextWriter);
public:
    TextWriter() = default;

    void append(char32_t character)
    {
        if (U_IS_BMP(character))
            m_builder.append(static_cast<char16_t>(character));
        else
            m_builder.append(character);
    }
    void append(const String& text) { m_builder.append(text); }
    // Where it has got to, to go back to.
    unsigned position() const { return m_builder.length(); }
    void goBackTo(unsigned position) { m_builder.shrink(position); }
    // A null String, having raised MemoryError, if there was no room.
    String finish(JSGlobalObject*);

private:
    StringBuilder m_builder { OverflowPolicy::RecordOverflow };
};

// The text of a str, or of an instance of a class derived from it. It is not null.
String textOfString(JSGlobalObject*, JSValue);

// _Py_error_handler: the handlers that a codec can do the work of by itself.
enum class ErrorHandler : uint8_t { Unknown, Strict, SurrogateEscape, Replace, Ignore, BackslashReplace, SurrogatePass, XMLCharRefReplace, Other };
ErrorHandler errorHandlerNamed(const String& errors); // _Py_GetErrorHandler()

// What a decoder does about what it cannot decode: it asks the handler of the name, which is any function that a program has registered. It lasts as long as the one call to the decoder, so the handler is
// looked up once and there is one exception, which is told each time where the trouble is now.
class DecodeErrors {
    WTF_MAKE_NONCOPYABLE(DecodeErrors);
    WTF_FORBID_HEAP_ALLOCATION;
public:
    DecodeErrors(JSGlobalObject* globalObject, const String& errors, std::span<const uint8_t> input)
        : input(input)
        , m_globalObject(globalObject)
        , m_errors(errors)
    {
    }

    // unicode_decode_call_errorhandler_writer(). What the handler gives in place of the bytes from `start` to `end` is written, and `position` is where it says to go on from. False if it raised.
    // The exception has a copy of the input, and the handler may put something else there: whatever is there afterwards is the input from then on.
    bool handle(ASCIILiteral encoding, ASCIILiteral reason, size_t start, size_t end, size_t& position, TextWriter&);

    // Whether it is still what the decoder was given, and not what an exception has.
    bool hasOriginalInput() const { return !m_exception; }

    std::span<const uint8_t> input;

private:
    JSGlobalObject* m_globalObject;
    const String& m_errors;
    JSValue m_handler;
    JSValue m_exception;
};

// The same for an encoder.
class EncodeErrors {
    WTF_MAKE_NONCOPYABLE(EncodeErrors);
    WTF_FORBID_HEAP_ALLOCATION;
public:
    EncodeErrors(JSGlobalObject* globalObject, const String& errors, JSValue string, size_t length)
        : m_globalObject(globalObject)
        , m_errors(errors)
        , m_string(string)
        , m_length(length)
    {
    }

    // unicode_encode_call_errorhandler(): what the handler gives in place of the characters from `start` to `end`, which is a bytes or a str, and where it says to go on from. Empty if it raised.
    JSValue handle(ASCIILiteral encoding, ASCIILiteral reason, size_t start, size_t end, size_t& newPosition);
    // raise_encode_exception()
    void raise(ASCIILiteral encoding, ASCIILiteral reason, size_t start, size_t end);

private:
    bool makeException(ASCIILiteral encoding, ASCIILiteral reason, size_t start, size_t end);

    JSGlobalObject* m_globalObject;
    const String& m_errors;
    JSValue m_string;
    size_t m_length;
    JSValue m_handler;
    JSValue m_exception;
};

// ---- The codecs

// Each decoder gives a null String if it raised. With `consumed`, what is at the end and may be the beginning of something is left, and that is how far it got.
String decodeUTF7(JSGlobalObject*, std::span<const uint8_t>, const String& errors, size_t* consumed = nullptr);
String decodeUTF8(JSGlobalObject*, std::span<const uint8_t>, const String& errors, size_t* consumed = nullptr);
// `byteOrder` is -1 for little-endian, 1 for big-endian, and 0 for whatever a mark at the beginning says, and is set to that.
String decodeUTF16(JSGlobalObject*, std::span<const uint8_t>, const String& errors, int* byteOrder = nullptr, size_t* consumed = nullptr);
String decodeUTF32(JSGlobalObject*, std::span<const uint8_t>, const String& errors, int* byteOrder = nullptr, size_t* consumed = nullptr);
String decodeUnicodeEscape(JSGlobalObject*, std::span<const uint8_t>, const String& errors, size_t* consumed = nullptr);
String decodeRawUnicodeEscape(JSGlobalObject*, std::span<const uint8_t>, const String& errors, size_t* consumed = nullptr);
String decodeLatin1(JSGlobalObject*, std::span<const uint8_t>);
String decodeASCII(JSGlobalObject*, std::span<const uint8_t>, const String& errors);
// Looking something up in a mapping can run anything, which can change the bytes, and that is seen. So it is asked each time where they are and how many there are.
String decodeCharmap(JSGlobalObject*, const Buffer&, JSValue mapping, const String& errors);
std::optional<ByteVector> decodeEscape(JSGlobalObject*, std::span<const uint8_t>, const String& errors); // PyBytes_DecodeEscape()

// Each encoder gives nothing if it raised.
std::optional<ByteVector> encodeUTF7(JSGlobalObject*, JSValue string);
std::optional<ByteVector> encodeUTF8(JSGlobalObject*, JSValue string, const String& errors);
std::optional<ByteVector> encodeUTF16(JSGlobalObject*, JSValue string, const String& errors, int byteOrder);
std::optional<ByteVector> encodeUTF32(JSGlobalObject*, JSValue string, const String& errors, int byteOrder);
std::optional<ByteVector> encodeUnicodeEscape(JSGlobalObject*, JSValue string);
std::optional<ByteVector> encodeRawUnicodeEscape(JSGlobalObject*, JSValue string);
std::optional<ByteVector> encodeLatin1(JSGlobalObject*, JSValue string, const String& errors);
std::optional<ByteVector> encodeASCII(JSGlobalObject*, JSValue string, const String& errors);
std::optional<ByteVector> encodeCharmap(JSGlobalObject*, JSValue string, JSValue mapping, const String& errors);
JSValue buildEncodingMap(JSGlobalObject*, JSValue string); // PyUnicode_BuildEncodingMap()

// The name of a character, and the character of a name, as unicodedata has them. Null, and nothing, if there is none.

// ---- The registry: Python/codecs.c

String normalizeEncodingName(const String&); // _Py_normalize_encoding()
void registerCodecSearchFunction(JSGlobalObject*, JSValue); // PyCodec_Register()
void unregisterCodecSearchFunction(JSGlobalObject*, JSValue); // PyCodec_Unregister()
// Each of these is empty if it raised.
JSValue lookupCodec(JSGlobalObject*, const String& encoding); // _PyCodec_Lookup()
JSValue lookupTextEncoding(JSGlobalObject*, const String& encoding, ASCIILiteral alternateCommand = { }); // _PyCodec_LookupTextEncoding()
JSValue makeIncrementalDecoder(JSGlobalObject*, JSValue codecInfo, const String& errors); // _PyCodecInfo_GetIncrementalDecoder()
JSValue makeIncrementalEncoder(JSGlobalObject*, JSValue codecInfo, const String& errors); // _PyCodecInfo_GetIncrementalEncoder()
JSValue encodeWithCodec(JSGlobalObject*, JSValue object, const String& encoding, const String& errors); // PyCodec_Encode()
JSValue decodeWithCodec(JSGlobalObject*, JSValue object, const String& encoding, const String& errors); // PyCodec_Decode()
JSValue encodeTextWithCodec(JSGlobalObject*, JSValue object, const String& encoding, const String& errors); // _PyCodec_EncodeText()
JSValue decodeTextWithCodec(JSGlobalObject*, JSValue object, const String& encoding, const String& errors); // _PyCodec_DecodeText()
void registerErrorHandler(JSGlobalObject*, const String& name, JSValue handler); // PyCodec_RegisterError()
std::optional<bool> unregisterErrorHandler(JSGlobalObject*, const String& name); // _PyCodec_UnregisterError()
JSValue lookupErrorHandler(JSGlobalObject*, const String& name); // PyCodec_LookupError(). A null String is "strict".
void setWhereUnicodeErrorIs(JSGlobalObject*, JSValue exception, size_t start, size_t end, ASCIILiteral reason); // PyUnicodeDecodeError_SetStart() and the rest, and the same for UnicodeEncodeError

} } // namespace JSC::Python
