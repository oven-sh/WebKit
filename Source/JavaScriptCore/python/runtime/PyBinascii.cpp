/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include "JSCInlines.h"
#include "PyBuffer.h"
#include "PyBytes.h"
#include "PyModule.h"
#include "PyModuleHelpers.h"
#include <array>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {

// The module binascii. (Modules/binascii.c in CPython is the reference.)

static constexpr TypeDef binasciiErrorTypeDef { "binascii.Error"_s, { TypeFlag::Base, TypeFlag::Mutable, TypeFlag::HasWeakref }, { }, { }, { } };
static constexpr TypeDef binasciiIncompleteTypeDef { "binascii.Incomplete"_s, { TypeFlag::Base, TypeFlag::Mutable, TypeFlag::HasWeakref }, { }, { }, { } };

static PyType* binasciiError(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);
    return rt.typeFor(binasciiErrorTypeDef, rt.ValueError());
}

template<typename... Parts>
static NEVER_INLINE JSValue binasciiRaise(JSGlobalObject* globalObject, Parts... parts)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    return raise(globalObject, scope, binasciiError(globalObject), makeString(parts...));
}

// ---- Arguments ----

// The bytes of an argument, which it keeps as they are for as long as this lives.
class BinasciiSource {
    WTF_MAKE_NONCOPYABLE(BinasciiSource);
    WTF_FORBID_HEAP_ALLOCATION;
public:
    BinasciiSource() = default;
    ~BinasciiSource()
    {
        if (m_globalObject)
            releaseBuffer(m_globalObject, m_buffer);
    }

    // For a parameter that takes any object with a buffer.
    bool setBuffer(JSGlobalObject* globalObject, JSValue value)
    {
        VM& vm = getVM(globalObject);
        auto scope = DECLARE_THROW_SCOPE(vm);
        if (failed(scope, getBuffer(globalObject, value, m_buffer)))
            return false;
        m_globalObject = globalObject;
        return true;
    }

    // For one that also takes a str, of ASCII characters.
    bool setASCII(JSGlobalObject* globalObject, JSValue value)
    {
        VM& vm = getVM(globalObject);
        auto scope = DECLARE_THROW_SCOPE(vm);
        if (isStr(value)) {
            m_text = strValue(value)->value(globalObject);
            RETURN_IF_EXCEPTION(scope, false);
            if (!m_text.containsOnlyASCII()) {
                raiseValueError(globalObject, scope, "string argument should contain only ASCII characters"_s);
                return false;
            }
            if (m_text.isNull())
                m_text = emptyString();
            else if (failed(scope, m_text.is8Bit()))
                m_text = String::make8Bit(m_text.span16());
            return true;
        }
        if (failed(scope, setBuffer(globalObject, value))) {
            takePendingException(globalObject);
            raiseTypeError(globalObject, scope, "argument should be bytes, buffer or ASCII string, not '"_s, typeName(globalObject, value), '\'');
            return false;
        }
        return true;
    }

    bool isSet() const { return m_globalObject || !m_text.isNull(); }

    // Converting the other arguments may have run code, and JS may have moved its memory then, or taken it away: a BufferError.
    bool refresh() { return !m_globalObject || refreshBuffer(m_globalObject, m_buffer); }

    std::span<const uint8_t> bytes() const
    {
        if (!m_text.isNull())
            return byteCast<uint8_t>(m_text.span8());
        return m_globalObject ? m_buffer.span() : std::span<const uint8_t>();
    }

private:
    JSGlobalObject* m_globalObject { nullptr };
    Buffer m_buffer;
    String m_text;
};

// An argument declared bool. Not given: what it is already.
static bool binasciiBoolArgument(JSGlobalObject* globalObject, JSValue value, bool& result)
{
    if (!value)
        return true;
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    result = isTrue(globalObject, value);
    return !scope.exception();
}

// An argument declared size_t.
static bool binasciiSizeArgument(JSGlobalObject* globalObject, JSValue value, size_t& result)
{
    if (!value)
        return true;
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue index = toIndex(globalObject, value);
    RETURN_IF_EXCEPTION(scope, false);
    if (intSign(index) < 0) {
        raiseValueError(globalObject, scope, "Cannot convert negative int"_s);
        return false;
    }
    auto converted = tryUInt64(index);
    if (!converted) {
        raiseOverflowError(globalObject, scope, "Python int too large for C size_t"_s);
        return false;
    }
    result = *converted;
    return true;
}

// An argument declared unsigned int of which only the low bits count.
static bool binasciiUnsignedArgument(JSGlobalObject* globalObject, JSValue value, uint32_t& result)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue index = toIndex(globalObject, value);
    RETURN_IF_EXCEPTION(scope, false);
    if (auto small = tryInt64(index); small && *small >= std::numeric_limits<int32_t>::min() && *small <= std::numeric_limits<uint32_t>::max()) {
        result = static_cast<uint32_t>(*small);
        return true;
    }
    bool proceed = warn(globalObject, rt.DeprecationWarning(), "integer value out of range"_s);
    RETURN_IF_EXCEPTION(scope, false);
    if (!proceed)
        return false;
    JSValue low = intAnd(globalObject, index, makeInt(globalObject, static_cast<uint32_t>(0xffffffff)));
    RETURN_IF_EXCEPTION(scope, false);
    result = static_cast<uint32_t>(tryInt64(low).value_or(0));
    return true;
}

// The alphabet of a decoder, which has to be a bytes.
static bool binasciiAlphabetArgument(JSGlobalObject* globalObject, ASCIILiteral function, JSValue value, BinasciiSource& result)
{
    if (!value)
        return true;
    if (PyBytes::is(value))
        return result.setBuffer(globalObject, value);
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    raiseTypeError(globalObject, scope, function, "() argument 'alphabet' must be bytes, not "_s, typeNameOfArgument(globalObject, value));
    return false;
}

static bool binasciiCheckAlphabetLength(JSGlobalObject* globalObject, std::span<const uint8_t> alphabet, unsigned size)
{
    if (alphabet.size() == size)
        return true;
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    raiseValueError(globalObject, scope, "alphabet must have length "_s, size);
    return false;
}

// ---- Tables ----

using BinasciiTable = std::array<uint8_t, 256>;

// From a character to its place in the alphabet. 255: it has none. The padding character, if any, is one past the last place.
static constexpr BinasciiTable binasciiReverseTable(std::span<const uint8_t> alphabet, int padding)
{
    BinasciiTable table { };
    for (auto& entry : table)
        entry = 255;
    for (size_t i = 0; i < alphabet.size(); ++i)
        table[alphabet[i]] = static_cast<uint8_t>(i);
    if (padding >= 0)
        table[padding] = static_cast<uint8_t>(alphabet.size());
    return table;
}

template<size_t size>
struct BinasciiAlphabet {
    constexpr BinasciiAlphabet(const char (&text)[size + 1])
    {
        for (size_t i = 0; i < size; ++i)
            characters[i] = static_cast<uint8_t>(text[i]);
    }
    constexpr std::span<const uint8_t> span() const { return { characters, size }; }
    uint8_t characters[size] { };
};

static constexpr BinasciiAlphabet<64> binasciiBase64Alphabet { "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/" };
static constexpr BinasciiAlphabet<85> binasciiBase85Alphabet { "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz!#$%&()*+-;<=>?@^_`{|}~" };
static constexpr BinasciiAlphabet<85> binasciiAscii85Alphabet { "!\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstu" };
static constexpr BinasciiAlphabet<32> binasciiBase32Alphabet { "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567" };

static constexpr uint32_t binasciiPowersOf85[] = { 1, 85, 7225, 614125, 52200625 };

// The characters a decoder skips.
class BinasciiIgnored {
public:
    BinasciiIgnored() = default;
    explicit BinasciiIgnored(std::span<const uint8_t> characters)
    {
        for (uint8_t character : characters)
            m_bits[character >> 6] |= 1ull << (character & 63);
    }
    bool contains(uint8_t character) const { return m_bits[character >> 6] & (1ull << (character & 63)); }

private:
    uint64_t m_bits[4] { };
};

static ALWAYS_INLINE int binasciiHexDigit(uint8_t character)
{
    if (character >= '0' && character <= '9')
        return character - '0';
    character |= 0x20;
    return character >= 'a' && character <= 'f' ? character - 'a' + 10 : -1;
}

// ---- Output ----

// A bytes of no more than a length known beforehand.
class BinasciiWriter {
public:
    // False: a MemoryError.
    bool allocate(JSGlobalObject* globalObject, size_t capacity)
    {
        m_bytes = newBytes(globalObject, capacity);
        return m_bytes;
    }
    uint8_t* start() { return m_bytes->mutableData(); }
    JSValue finish(JSGlobalObject* globalObject, uint8_t* end)
    {
        size_t length = end - start();
        ASSERT(length <= m_bytes->length());
        if (length == m_bytes->length())
            return m_bytes;
        return PyBytes::create(globalObject, m_bytes->span().first(length));
    }

private:
    PyBytes* m_bytes { nullptr };
};

// Breaks the bytes into lines of that width. There is room for the newlines. Returns how many bytes there are then.
static size_t binasciiWrapLines(uint8_t* data, size_t size, size_t width)
{
    if (size <= width)
        return size;
    size_t newlines = (size - 1) / width;
    for (size_t line = newlines; line; --line) {
        size_t start = line * width;
        memmove(data + start + line, data + start, std::min(width, size - start));
        data[start + line - 1] = '\n';
    }
    return size + newlines;
}

// ---- uuencode ----

static JSValue binasciiA2BUU(JSGlobalObject* globalObject, JSValue dataValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    BinasciiSource data;
    if (failed(scope, data.setASCII(globalObject, dataValue)))
        return { };
    auto input = data.bytes();
    if (input.empty())
        RELEASE_AND_RETURN(scope, binasciiRaise(globalObject, "Missing length byte"_s));
    // The first character says how many bytes the line holds.
    size_t length = (input[0] - ' ') & 077;
    BinasciiWriter writer;
    if (!writer.allocate(globalObject, length))
        return { };
    uint8_t* out = writer.start();
    uint8_t* end = out + length;
    size_t position = 1;
    unsigned bits = 0;
    unsigned bitCount = 0;
    for (; out < end; ++position) {
        // Spaces at the end of the line may have been lost on the way: what is missing is zeros.
        uint8_t character = position < input.size() ? input[position] : 0;
        if (character == '\n' || character == '\r' || position >= input.size())
            character = 0;
        else {
            // One more than the 64 there should be: some encoders have '`' for zero.
            if (character < ' ' || character > ' ' + 64)
                RELEASE_AND_RETURN(scope, binasciiRaise(globalObject, "Illegal char"_s));
            character = (character - ' ') & 077;
        }
        bits = (bits << 6) | character;
        bitCount += 6;
        if (bitCount >= 8) {
            bitCount -= 8;
            *out++ = static_cast<uint8_t>(bits >> bitCount);
            bits &= (1u << bitCount) - 1;
        }
    }
    for (; position < input.size(); ++position) {
        uint8_t character = input[position];
        if (character != ' ' && character != ' ' + 64 && character != '\n' && character != '\r')
            RELEASE_AND_RETURN(scope, binasciiRaise(globalObject, "Trailing garbage"_s));
    }
    RELEASE_AND_RETURN(scope, writer.finish(globalObject, out));
}

static JSValue binasciiB2AUU(JSGlobalObject* globalObject, JSValue dataValue, JSValue backtickValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    BinasciiSource data;
    bool backtick = false;
    if (failed(scope, data.setBuffer(globalObject, dataValue)) || failed(scope, binasciiBoolArgument(globalObject, backtickValue, backtick))
        || !data.refresh())
        return { };
    auto input = data.bytes();
    if (input.size() > 45)
        RELEASE_AND_RETURN(scope, binasciiRaise(globalObject, "At most 45 bytes at once"_s));
    BinasciiWriter writer;
    if (!writer.allocate(globalObject, 2 + (input.size() + 2) / 3 * 4))
        return { };
    uint8_t* out = writer.start();
    auto encode = [&](unsigned sextet) -> uint8_t {
        return backtick && !sextet ? '`' : static_cast<uint8_t>(' ' + sextet);
    };
    *out++ = encode(static_cast<unsigned>(input.size()));
    for (size_t i = 0; i < input.size(); i += 3) {
        unsigned group = input[i] << 16 | (i + 1 < input.size() ? input[i + 1] << 8 : 0) | (i + 2 < input.size() ? input[i + 2] : 0);
        for (int shift = 18; shift >= 0; shift -= 6)
            *out++ = encode((group >> shift) & 077);
    }
    *out++ = '\n';
    RELEASE_AND_RETURN(scope, writer.finish(globalObject, out));
}

// ---- Base64 ----

static JSValue binasciiA2BBase64(JSGlobalObject* globalObject, JSValue dataValue, JSValue strictValue, JSValue paddedValue, JSValue alphabetValue, JSValue ignoreValue, JSValue canonicalValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    BinasciiSource data;
    BinasciiSource alphabet;
    BinasciiSource ignoreSource;
    bool isStrict = false;
    bool isPadded = true;
    bool isCanonical = false;
    if (failed(scope, data.setASCII(globalObject, dataValue))
        || failed(scope, binasciiBoolArgument(globalObject, strictValue, isStrict))
        || failed(scope, binasciiBoolArgument(globalObject, paddedValue, isPadded))
        || failed(scope, binasciiAlphabetArgument(globalObject, "a2b_base64"_s, alphabetValue, alphabet))
        || (ignoreValue && failed(scope, ignoreSource.setBuffer(globalObject, ignoreValue)))
        || failed(scope, binasciiBoolArgument(globalObject, canonicalValue, isCanonical))
        || !data.refresh() || !alphabet.refresh() || !ignoreSource.refresh())
        return { };
    // To say what to ignore is to say that nothing else is to be.
    if (!strictValue)
        isStrict = ignoreSource.isSet();
    BinasciiIgnored ignored;
    if (isStrict)
        ignored = BinasciiIgnored(ignoreSource.bytes());

    static constexpr BinasciiTable standardTable = binasciiReverseTable(binasciiBase64Alphabet.span(), '=');
    BinasciiTable customTable;
    const uint8_t* table = standardTable.data();
    if (alphabet.isSet()) {
        if (failed(scope, binasciiCheckAlphabetLength(globalObject, alphabet.bytes(), 64)))
            return { };
        customTable = binasciiReverseTable(alphabet.bytes(), '=');
        table = customTable.data();
    }

    auto input = data.bytes();
    BinasciiWriter writer;
    if (!writer.allocate(globalObject, (input.size() + 3) / 4 * 3))
        return { };
    uint8_t* out = writer.start();
    const uint8_t* in = input.data();
    const uint8_t* end = in + input.size();

    unsigned position = 0; // In the group of four characters.
    unsigned pads = 0;
    uint8_t pending = 0; // The bits of the last character that no byte has taken yet.
    while (in < end) {
        // Whole groups with nothing special in them.
        if (!position) {
            for (; end - in >= 4; in += 4, out += 3) {
                uint8_t a = table[in[0]];
                uint8_t b = table[in[1]];
                uint8_t c = table[in[2]];
                uint8_t d = table[in[3]];
                if ((a | b | c | d) & 0xc0)
                    break;
                out[0] = static_cast<uint8_t>(a << 2 | b >> 4);
                out[1] = static_cast<uint8_t>(b << 4 | c >> 2);
                out[2] = static_cast<uint8_t>(c << 6 | d);
                pads = 0;
            }
            if (in == end)
                break;
        }
        uint8_t character = *in++;
        if (isPadded && character == '=') {
            ++pads;
            if (position >= 2 && position + pads <= 4)
                continue;
            // Padding where there should be none may be taken for a character that is not in the alphabet. (RFC 4648, 3.3.)
            if (!isStrict || ignored.contains('='))
                continue;
            if (position == 1)
                break;
            RELEASE_AND_RETURN(scope, binasciiRaise(globalObject, !position && out == writer.start() ? "Leading padding not allowed"_s : "Excess padding not allowed"_s));
        }
        uint8_t value = table[character];
        if (value >= 64) {
            if (isStrict && !ignored.contains(character))
                RELEASE_AND_RETURN(scope, binasciiRaise(globalObject, character == '=' ? "Padding not allowed"_s : "Only base64 data is allowed"_s));
            continue;
        }
        if (pads && isStrict && !ignored.contains('='))
            RELEASE_AND_RETURN(scope, binasciiRaise(globalObject, position + pads == 4 ? "Excess data after padding"_s : "Discontinuous padding not allowed"_s));
        pads = 0;
        switch (position) {
        case 0:
            pending = value;
            break;
        case 1:
            *out++ = static_cast<uint8_t>(pending << 2 | value >> 4);
            pending = value & 0x0f;
            break;
        case 2:
            *out++ = static_cast<uint8_t>(pending << 4 | value >> 2);
            pending = value & 0x03;
            break;
        default:
            *out++ = static_cast<uint8_t>(pending << 6 | value);
            pending = 0;
            break;
        }
        position = (position + 1) & 3;
    }

    if (position == 1) {
        return binasciiRaise(globalObject, "Invalid base64-encoded string: number of data characters ("_s, static_cast<size_t>(out - writer.start()) / 3 * 4 + 1,
            ") cannot be 1 more than a multiple of 4"_s);
    }
    if (isPadded && position && position + pads < 4)
        RELEASE_AND_RETURN(scope, binasciiRaise(globalObject, "Incorrect padding"_s));
    if (isCanonical && pending)
        RELEASE_AND_RETURN(scope, binasciiRaise(globalObject, "Non-zero padding bits"_s));
    RELEASE_AND_RETURN(scope, writer.finish(globalObject, out));
}

static JSValue binasciiB2ABase64(JSGlobalObject* globalObject, JSValue dataValue, JSValue paddedValue, JSValue wrapValue, JSValue newlineValue, JSValue alphabetValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    BinasciiSource data;
    BinasciiSource alphabet;
    bool isPadded = true;
    bool hasNewline = true;
    size_t width = 0;
    if (failed(scope, data.setBuffer(globalObject, dataValue))
        || failed(scope, binasciiBoolArgument(globalObject, paddedValue, isPadded))
        || failed(scope, binasciiSizeArgument(globalObject, wrapValue, width))
        || failed(scope, binasciiBoolArgument(globalObject, newlineValue, hasNewline))
        || (alphabetValue && failed(scope, alphabet.setBuffer(globalObject, alphabetValue)))
        || !data.refresh() || !alphabet.refresh())
        return { };
    const uint8_t* table = binasciiBase64Alphabet.characters;
    if (alphabet.isSet()) {
        if (failed(scope, binasciiCheckAlphabetLength(globalObject, alphabet.bytes(), 64)))
            return { };
        table = alphabet.bytes().data();
    }
    auto input = data.bytes();
    size_t length = (input.size() + 2) / 3 * 4;
    unsigned pads = (3 - input.size() % 3) % 3;
    if (!isPadded) {
        length -= pads;
        pads = 0;
    }
    if (width && length) {
        // A line holds a whole number of groups.
        width = width < 4 ? 4 : width / 4 * 4;
        length += (length - 1) / width;
    }
    BinasciiWriter writer;
    if (!writer.allocate(globalObject, length + hasNewline))
        return { };
    uint8_t* out = writer.start();
    const uint8_t* in = input.data();
    size_t left = input.size();
    for (; left >= 3; left -= 3, in += 3, out += 4) {
        unsigned group = in[0] << 16 | in[1] << 8 | in[2];
        out[0] = table[group >> 18];
        out[1] = table[(group >> 12) & 0x3f];
        out[2] = table[(group >> 6) & 0x3f];
        out[3] = table[group & 0x3f];
    }
    if (left == 1) {
        *out++ = table[in[0] >> 2];
        *out++ = table[(in[0] << 4) & 0x3f];
    } else if (left == 2) {
        unsigned group = in[0] << 8 | in[1];
        *out++ = table[group >> 10];
        *out++ = table[(group >> 4) & 0x3f];
        *out++ = table[(group << 2) & 0x3f];
    }
    for (; pads; --pads)
        *out++ = '=';
    if (width)
        out = writer.start() + binasciiWrapLines(writer.start(), out - writer.start(), width);
    if (hasNewline)
        *out++ = '\n';
    RELEASE_AND_RETURN(scope, writer.finish(globalObject, out));
}

// ---- Ascii85 and Base85 ----

// What the two decoders have in common, once they have five digits, or what is left of the input filled up with the largest
// digit. `missing` is how many digits were filled in. Null: all is well.
static ASCIILiteral binasciiWrite85Group(uint32_t group, unsigned missing, bool isCanonical, ASCIILiteral incompleteMessage, uint8_t*& out)
{
    // A single digit cannot be the end of any encoding.
    if (missing == 4)
        return incompleteMessage;
    unsigned count = 4 - missing;
    for (unsigned i = 0; i < count; ++i)
        *out++ = static_cast<uint8_t>(group >> (24 - 8 * i));
    if (isCanonical && missing) {
        // An encoder fills the bytes up with zeros and keeps the leading digits: they have to be the ones that were given.
        uint32_t zeroFilled = (group >> (missing * 8)) << (missing * 8);
        if (zeroFilled / binasciiPowersOf85[missing] != group / binasciiPowersOf85[missing])
            return "Non-zero padding bits"_s;
    }
    return { };
}

static ALWAYS_INLINE bool binasciiAdd85Digit(uint32_t& group, unsigned position, uint8_t digit)
{
    if (position == 4 && (group > std::numeric_limits<uint32_t>::max() / 85 || group * 85 > std::numeric_limits<uint32_t>::max() - digit))
        return false;
    group = group * 85 + digit;
    return true;
}

static JSValue binasciiA2BAscii85(JSGlobalObject* globalObject, JSValue dataValue, JSValue foldValue, JSValue adobeValue, JSValue ignoreValue, JSValue canonicalValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    BinasciiSource data;
    BinasciiSource ignoreSource;
    bool foldSpaces = false;
    bool isAdobe = false;
    bool isCanonical = false;
    if (failed(scope, data.setASCII(globalObject, dataValue))
        || failed(scope, binasciiBoolArgument(globalObject, foldValue, foldSpaces))
        || failed(scope, binasciiBoolArgument(globalObject, adobeValue, isAdobe))
        || (ignoreValue && failed(scope, ignoreSource.setBuffer(globalObject, ignoreValue)))
        || failed(scope, binasciiBoolArgument(globalObject, canonicalValue, isCanonical))
        || !data.refresh() || !ignoreSource.refresh())
        return { };
    BinasciiIgnored ignored(ignoreSource.bytes());
    auto input = data.bytes();
    if (isAdobe) {
        if (input.size() < 2 || input[input.size() - 2] != '~' || input[input.size() - 1] != '>')
            RELEASE_AND_RETURN(scope, binasciiRaise(globalObject, "Ascii85 encoded byte sequences must end with b'~>'"_s));
        input = input.first(input.size() - 2);
        if (input.size() >= 2 && input[0] == '<' && input[1] == '~')
            input = input.subspan(2);
    }

    // 'y' and 'z' stand for four bytes each.
    size_t shorthands = 0;
    for (uint8_t character : input)
        shorthands += character == 'y' || character == 'z';
    BinasciiWriter writer;
    if (!writer.allocate(globalObject, (input.size() - shorthands + 4) / 5 * 4 + shorthands * 4))
        return { };
    uint8_t* out = writer.start();

    static constexpr BinasciiTable table = binasciiReverseTable(binasciiAscii85Alphabet.span(), -1);
    uint32_t group = 0;
    unsigned position = 0;
    auto flush = [&](unsigned missing, bool isShorthandForZero) -> JSValue {
        if (ASCIILiteral message = binasciiWrite85Group(group, missing, isCanonical, "Incomplete Ascii85 group"_s, out); !message.isNull())
            return binasciiRaise(globalObject, message);
        if (isCanonical && !missing && !group && !isShorthandForZero)
            return binasciiRaise(globalObject, "Non-canonical encoding, use 'z' for all-zero groups"_s);
        group = 0;
        position = 0;
        return jsUndefined();
    };
    for (uint8_t character : input) {
        uint8_t digit = table[character];
        bool isShorthandForZero = false;
        if (digit < 85) {
            if (!binasciiAdd85Digit(group, position, digit))
                RELEASE_AND_RETURN(scope, binasciiRaise(globalObject, "Ascii85 overflow"_s));
            ++position;
        } else if ((character == 'y' && foldSpaces) || character == 'z') {
            if (position)
                RELEASE_AND_RETURN(scope, binasciiRaise(globalObject, '\'', static_cast<char>(character), "' inside Ascii85 5-tuple"_s));
            group = character == 'y' ? 0x20202020 : 0;
            isShorthandForZero = character == 'z';
            position = 5;
        } else if (!ignored.contains(character))
            RELEASE_AND_RETURN(scope, binasciiRaise(globalObject, "Non-Ascii85 digit found: "_s, static_cast<char16_t>(character)));
        if (position == 5 && !flush(0, isShorthandForZero))
            return { };
    }
    if (position) {
        unsigned missing = 5 - position;
        for (; position < 5; ++position) {
            if (!binasciiAdd85Digit(group, position, 84))
                RELEASE_AND_RETURN(scope, binasciiRaise(globalObject, "Ascii85 overflow"_s));
        }
        if (!flush(missing, false))
            return { };
    }
    RELEASE_AND_RETURN(scope, writer.finish(globalObject, out));
}

// The five digits of a group, or the first `count` of them.
static ALWAYS_INLINE void binasciiEncode85Group(const uint8_t* table, uint32_t group, unsigned count, uint8_t*& out)
{
    for (unsigned i = 5; i--; group /= 85) {
        if (i < count)
            out[i] = table[group % 85];
    }
    out += count;
}

static ALWAYS_INLINE uint32_t binasciiLoad85Group(const uint8_t* in, size_t available)
{
    uint32_t group = 0;
    for (size_t i = 0; i < 4; ++i)
        group = group << 8 | (i < available ? in[i] : 0);
    return group;
}

static JSValue binasciiB2AAscii85(JSGlobalObject* globalObject, JSValue dataValue, JSValue foldValue, JSValue wrapValue, JSValue padValue, JSValue adobeValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    BinasciiSource data;
    bool foldSpaces = false;
    bool pad = false;
    bool isAdobe = false;
    size_t width = 0;
    if (failed(scope, data.setBuffer(globalObject, dataValue))
        || failed(scope, binasciiBoolArgument(globalObject, foldValue, foldSpaces))
        || failed(scope, binasciiSizeArgument(globalObject, wrapValue, width))
        || failed(scope, binasciiBoolArgument(globalObject, padValue, pad))
        || failed(scope, binasciiBoolArgument(globalObject, adobeValue, isAdobe))
        || !data.refresh())
        return { };
    auto input = data.bytes();
    if (isAdobe && width == 1)
        width = 2;
    size_t length = (input.size() + 3) / 4 * 5 + (isAdobe ? 4 : 0);
    if (!pad && input.size() % 4)
        length -= 4 - input.size() % 4;
    size_t capacity = length;
    if (width && length)
        capacity += (length - 1) / width;
    BinasciiWriter writer;
    if (!writer.allocate(globalObject, capacity))
        return { };
    uint8_t* out = writer.start();
    if (isAdobe) {
        *out++ = '<';
        *out++ = '~';
    }
    const uint8_t* table = binasciiAscii85Alphabet.characters;
    const uint8_t* in = input.data();
    size_t left = input.size();
    for (; left >= 4; left -= 4, in += 4) {
        uint32_t group = binasciiLoad85Group(in, 4);
        if (!group)
            *out++ = 'z';
        else if (foldSpaces && group == 0x20202020)
            *out++ = 'y';
        else
            binasciiEncode85Group(table, group, 5, out);
    }
    if (left) {
        uint32_t group = binasciiLoad85Group(in, left);
        if (pad && !group)
            *out++ = 'z';
        else
            binasciiEncode85Group(table, group, pad ? 5 : static_cast<unsigned>(left) + 1, out);
    }
    if (isAdobe) {
        *out++ = '~';
        *out++ = '>';
    }
    if (width && length) {
        out = writer.start() + binasciiWrapLines(writer.start(), out - writer.start(), width);
        // The two characters of the end stay together.
        if (isAdobe && out[-2] == '\n') {
            out[-3] = '\n';
            out[-2] = '~';
        }
    }
    RELEASE_AND_RETURN(scope, writer.finish(globalObject, out));
}

static JSValue binasciiA2BBase85(JSGlobalObject* globalObject, JSValue dataValue, JSValue alphabetValue, JSValue ignoreValue, JSValue canonicalValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    BinasciiSource data;
    BinasciiSource alphabet;
    BinasciiSource ignoreSource;
    bool isCanonical = false;
    if (failed(scope, data.setASCII(globalObject, dataValue))
        || failed(scope, binasciiAlphabetArgument(globalObject, "a2b_base85"_s, alphabetValue, alphabet))
        || (ignoreValue && failed(scope, ignoreSource.setBuffer(globalObject, ignoreValue)))
        || failed(scope, binasciiBoolArgument(globalObject, canonicalValue, isCanonical))
        || !data.refresh() || !alphabet.refresh() || !ignoreSource.refresh())
        return { };
    static constexpr BinasciiTable standardTable = binasciiReverseTable(binasciiBase85Alphabet.span(), -1);
    BinasciiTable customTable;
    const uint8_t* table = standardTable.data();
    if (alphabet.isSet()) {
        if (failed(scope, binasciiCheckAlphabetLength(globalObject, alphabet.bytes(), 85)))
            return { };
        customTable = binasciiReverseTable(alphabet.bytes(), -1);
        table = customTable.data();
    }
    BinasciiIgnored ignored(ignoreSource.bytes());
    auto input = data.bytes();
    BinasciiWriter writer;
    if (!writer.allocate(globalObject, (input.size() + 4) / 5 * 4))
        return { };
    uint8_t* out = writer.start();

    uint32_t group = 0;
    unsigned position = 0;
    // Past the end of the input, the group is filled up with the largest digit.
    for (size_t i = 0; i < input.size() || position; ++i) {
        uint8_t digit = i < input.size() ? table[input[i]] : 84;
        if (digit < 85) {
            if (!binasciiAdd85Digit(group, position, digit))
                RELEASE_AND_RETURN(scope, binasciiRaise(globalObject, "Base85 overflow in hunk starting at byte "_s, i / 5 * 5));
            ++position;
        } else if (!ignored.contains(input[i]))
            RELEASE_AND_RETURN(scope, binasciiRaise(globalObject, "bad Base85 character at position "_s, i));
        if (position != 5)
            continue;
        unsigned missing = i < input.size() ? 0 : static_cast<unsigned>(i - input.size() + 1);
        if (ASCIILiteral message = binasciiWrite85Group(group, missing, isCanonical, "Incomplete Base85 group"_s, out); !message.isNull())
            RELEASE_AND_RETURN(scope, binasciiRaise(globalObject, message));
        group = 0;
        position = 0;
    }
    RELEASE_AND_RETURN(scope, writer.finish(globalObject, out));
}

static JSValue binasciiB2ABase85(JSGlobalObject* globalObject, JSValue dataValue, JSValue padValue, JSValue wrapValue, JSValue alphabetValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    BinasciiSource data;
    BinasciiSource alphabet;
    bool pad = false;
    size_t width = 0;
    if (failed(scope, data.setBuffer(globalObject, dataValue))
        || failed(scope, binasciiBoolArgument(globalObject, padValue, pad))
        || failed(scope, binasciiSizeArgument(globalObject, wrapValue, width))
        || (alphabetValue && failed(scope, alphabet.setBuffer(globalObject, alphabetValue)))
        || !data.refresh() || !alphabet.refresh())
        return { };
    const uint8_t* table = binasciiBase85Alphabet.characters;
    if (alphabet.isSet()) {
        if (failed(scope, binasciiCheckAlphabetLength(globalObject, alphabet.bytes(), 85)))
            return { };
        table = alphabet.bytes().data();
    }
    auto input = data.bytes();
    size_t length = (input.size() + 3) / 4 * 5;
    if (!pad && input.size() % 4)
        length -= 4 - input.size() % 4;
    size_t capacity = length;
    if (width && length) {
        width = width < 5 ? 5 : width / 5 * 5;
        capacity += (length - 1) / width;
    }
    BinasciiWriter writer;
    if (!writer.allocate(globalObject, capacity))
        return { };
    uint8_t* out = writer.start();
    const uint8_t* in = input.data();
    size_t left = input.size();
    for (; left >= 4; left -= 4, in += 4)
        binasciiEncode85Group(table, binasciiLoad85Group(in, 4), 5, out);
    if (left)
        binasciiEncode85Group(table, binasciiLoad85Group(in, left), pad ? 5 : static_cast<unsigned>(left) + 1, out);
    if (width && length)
        out = writer.start() + binasciiWrapLines(writer.start(), out - writer.start(), width);
    RELEASE_AND_RETURN(scope, writer.finish(globalObject, out));
}

// ---- Base32 ----

static JSValue binasciiA2BBase32(JSGlobalObject* globalObject, JSValue dataValue, JSValue paddedValue, JSValue alphabetValue, JSValue ignoreValue, JSValue canonicalValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    BinasciiSource data;
    BinasciiSource alphabet;
    BinasciiSource ignoreSource;
    bool isPadded = true;
    bool isCanonical = false;
    if (failed(scope, data.setASCII(globalObject, dataValue))
        || failed(scope, binasciiBoolArgument(globalObject, paddedValue, isPadded))
        || failed(scope, binasciiAlphabetArgument(globalObject, "a2b_base32"_s, alphabetValue, alphabet))
        || (ignoreValue && failed(scope, ignoreSource.setBuffer(globalObject, ignoreValue)))
        || failed(scope, binasciiBoolArgument(globalObject, canonicalValue, isCanonical))
        || !data.refresh() || !alphabet.refresh() || !ignoreSource.refresh())
        return { };
    static constexpr BinasciiTable standardTable = binasciiReverseTable(binasciiBase32Alphabet.span(), '=');
    BinasciiTable customTable;
    const uint8_t* table = standardTable.data();
    if (alphabet.isSet()) {
        if (failed(scope, binasciiCheckAlphabetLength(globalObject, alphabet.bytes(), 32)))
            return { };
        customTable = binasciiReverseTable(alphabet.bytes(), '=');
        table = customTable.data();
    }
    BinasciiIgnored ignored(ignoreSource.bytes());
    auto input = data.bytes();
    BinasciiWriter writer;
    if (!writer.allocate(globalObject, (input.size() + 7) / 8 * 5))
        return { };
    uint8_t* out = writer.start();

    // A group of eight characters is five bytes. After 1, 3 or 6 characters there are bits that begin a byte and no more.
    auto cannotEndAt = [](unsigned position) { return position == 1 || position == 3 || position == 6; };
    unsigned position = 0;
    unsigned pads = 0;
    unsigned bits = 0; // Those that no byte has taken yet.
    unsigned bitCount = 0;
    for (uint8_t character : input) {
        if (isPadded && character == '=') {
            ++pads;
            if (position && !cannotEndAt(position) && position + pads <= 8)
                continue;
            if (ignored.contains('='))
                continue;
            if (cannotEndAt(position))
                break;
            RELEASE_AND_RETURN(scope, binasciiRaise(globalObject, !position && out == writer.start() ? "Leading padding not allowed"_s : "Excess padding not allowed"_s));
        }
        uint8_t value = table[character];
        if (value >= 32) {
            if (!ignored.contains(character))
                RELEASE_AND_RETURN(scope, binasciiRaise(globalObject, character == '=' ? "Padding not allowed"_s : "Only base32 data is allowed"_s));
            continue;
        }
        if (pads && !ignored.contains('='))
            RELEASE_AND_RETURN(scope, binasciiRaise(globalObject, position + pads == 8 ? "Excess data after padding"_s : "Discontinuous padding not allowed"_s));
        bits = bits << 5 | value;
        bitCount += 5;
        if (bitCount >= 8) {
            bitCount -= 8;
            *out++ = static_cast<uint8_t>(bits >> bitCount);
            bits &= (1u << bitCount) - 1;
        }
        position = (position + 1) & 7;
        // Padding that was ignored counts until the group it is in ends.
        if (!position)
            pads = 0;
    }

    if (cannotEndAt(position)) {
        return binasciiRaise(globalObject, "Invalid base32-encoded string: number of data characters ("_s, static_cast<size_t>(out - writer.start()) / 5 * 8 + position,
            ") cannot be 1, 3, or 6 more than a multiple of 8"_s);
    }
    if (isPadded && position && position + pads < 8)
        RELEASE_AND_RETURN(scope, binasciiRaise(globalObject, "Incorrect padding"_s));
    if (isCanonical && bits)
        RELEASE_AND_RETURN(scope, binasciiRaise(globalObject, "Non-zero padding bits"_s));
    RELEASE_AND_RETURN(scope, writer.finish(globalObject, out));
}

static JSValue binasciiB2ABase32(JSGlobalObject* globalObject, JSValue dataValue, JSValue paddedValue, JSValue wrapValue, JSValue alphabetValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    BinasciiSource data;
    BinasciiSource alphabet;
    bool isPadded = true;
    size_t width = 0;
    if (failed(scope, data.setBuffer(globalObject, dataValue))
        || failed(scope, binasciiBoolArgument(globalObject, paddedValue, isPadded))
        || failed(scope, binasciiSizeArgument(globalObject, wrapValue, width))
        || (alphabetValue && failed(scope, alphabet.setBuffer(globalObject, alphabetValue)))
        || !data.refresh() || !alphabet.refresh())
        return { };
    const uint8_t* table = binasciiBase32Alphabet.characters;
    if (alphabet.isSet()) {
        if (failed(scope, binasciiCheckAlphabetLength(globalObject, alphabet.bytes(), 32)))
            return { };
        table = alphabet.bytes().data();
    }
    auto input = data.bytes();
    size_t length = (input.size() + 4) / 5 * 8;
    unsigned pads = (5 - input.size() % 5) % 5 * 8 / 5;
    if (!isPadded) {
        length -= pads;
        pads = 0;
    }
    size_t capacity = length;
    if (width && length) {
        width = width < 8 ? 8 : width / 8 * 8;
        capacity += (length - 1) / width;
    }
    BinasciiWriter writer;
    if (!writer.allocate(globalObject, capacity))
        return { };
    uint8_t* out = writer.start();
    uint64_t bits = 0;
    unsigned bitCount = 0;
    for (uint8_t byte : input) {
        bits = (bits << 8 | byte) & 0xfff;
        bitCount += 8;
        while (bitCount >= 5) {
            bitCount -= 5;
            *out++ = table[(bits >> bitCount) & 0x1f];
        }
    }
    if (bitCount)
        *out++ = table[(bits << (5 - bitCount)) & 0x1f];
    for (; pads; --pads)
        *out++ = '=';
    if (width && length)
        out = writer.start() + binasciiWrapLines(writer.start(), out - writer.start(), width);
    RELEASE_AND_RETURN(scope, writer.finish(globalObject, out));
}

// ---- Checksums ----

// CRC-CCITT: x^16 + x^12 + x^5 + 1, the high bit first.
static constexpr std::array<uint16_t, 256> binasciiMakeCCITTTable()
{
    std::array<uint16_t, 256> table { };
    for (unsigned i = 0; i < 256; ++i) {
        unsigned crc = i << 8;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc << 1) ^ (crc & 0x8000 ? 0x1021 : 0);
        table[i] = static_cast<uint16_t>(crc);
    }
    return table;
}

// CRC-32, as in zlib: the polynomial 0xedb88320, the low bit first.
static constexpr std::array<uint32_t, 256> binasciiMakeCRC32Table()
{
    std::array<uint32_t, 256> table { };
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t crc = i;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (crc & 1 ? 0xedb88320u : 0);
        table[i] = crc;
    }
    return table;
}

static JSValue binasciiCRCHqx(JSGlobalObject* globalObject, JSValue dataValue, JSValue crcValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    static constexpr auto table = binasciiMakeCCITTTable();
    BinasciiSource data;
    uint32_t crc;
    if (failed(scope, data.setBuffer(globalObject, dataValue)) || !binasciiUnsignedArgument(globalObject, crcValue, crc)
        || !data.refresh())
        return { };
    crc &= 0xffff;
    for (uint8_t byte : data.bytes())
        crc = ((crc << 8) & 0xff00) ^ table[(crc >> 8) ^ byte];
    return jsNumber(crc);
}

static JSValue binasciiCRC32(JSGlobalObject* globalObject, JSValue dataValue, JSValue crcValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    static constexpr auto table = binasciiMakeCRC32Table();
    BinasciiSource data;
    uint32_t crc = 0;
    if (failed(scope, data.setBuffer(globalObject, dataValue)) || (crcValue && !binasciiUnsignedArgument(globalObject, crcValue, crc))
        || !data.refresh())
        return { };
    crc = ~crc;
    for (uint8_t byte : data.bytes())
        crc = table[(crc ^ byte) & 0xff] ^ (crc >> 8);
    return jsNumber(~crc);
}

// ---- Hexadecimal ----

static JSValue binasciiB2AHex(JSGlobalObject* globalObject, JSValue dataValue, JSValue separatorValue, JSValue groupValue)
{
    static constexpr char digits[] = "0123456789abcdef";
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    BinasciiSource data;
    int64_t group = 1;
    if (failed(scope, data.setBuffer(globalObject, dataValue)) || (groupValue && failed(scope, ssizeArgument(globalObject, groupValue, group))))
        return { };
    uint8_t separator = 0;
    if (separatorValue) {
        int64_t separatorLength = length(globalObject, separatorValue);
        RETURN_IF_EXCEPTION(scope, { });
        if (separatorLength != 1)
            return raiseValueError(globalObject, scope, "sep must be length 1."_s);
        if (isStr(separatorValue)) {
            StrView view(globalObject, strValue(separatorValue));
            RETURN_IF_EXCEPTION(scope, { });
            if (view.at(0) > 0xff)
                return raiseValueError(globalObject, scope, "sep must be ASCII."_s);
            separator = static_cast<uint8_t>(view.at(0));
        } else if (PyBytes::is(separatorValue))
            separator = PyBytes::from(separatorValue)->span()[0];
        else
            return raiseTypeError(globalObject, scope, "sep must be str or bytes."_s);
    } else
        group = 0;
    if (!data.refresh())
        return { };

    auto input = data.bytes();
    size_t size = input.size();
    // Counted from the end if positive, from the start if negative.
    bool fromStart = group < 0;
    uint64_t groupSize = group < 0 ? -static_cast<uint64_t>(group) : group;
    if (groupSize >= size)
        groupSize = 0;
    PyBytes* result = newBytes(globalObject, size * 2 + (groupSize ? (size - 1) / groupSize : 0));
    if (!result)
        return { };
    uint8_t* out = result->mutableData();
    for (size_t i = 0; i < size; ++i) {
        if (groupSize && i && !((fromStart ? i : size - i) % groupSize))
            *out++ = separator;
        *out++ = digits[input[i] >> 4];
        *out++ = digits[input[i] & 15];
    }
    return result;
}

static JSValue binasciiA2BHex(JSGlobalObject* globalObject, JSValue dataValue, JSValue ignoreValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    BinasciiSource data;
    BinasciiSource ignoreSource;
    if (failed(scope, data.setASCII(globalObject, dataValue)) || (ignoreValue && failed(scope, ignoreSource.setBuffer(globalObject, ignoreValue)))
        || !data.refresh() || !ignoreSource.refresh())
        return { };
    BinasciiIgnored ignored(ignoreSource.bytes());
    auto input = data.bytes();
    BinasciiWriter writer;
    if (!writer.allocate(globalObject, input.size() / 2))
        return { };
    uint8_t* out = writer.start();
    int high = -1;
    for (uint8_t character : input) {
        int digit = binasciiHexDigit(character);
        if (digit < 0) {
            if (!ignored.contains(character))
                RELEASE_AND_RETURN(scope, binasciiRaise(globalObject, "Non-hexadecimal digit found"_s));
            continue;
        }
        if (high < 0)
            high = digit;
        else {
            *out++ = static_cast<uint8_t>(high << 4 | digit);
            high = -1;
        }
    }
    if (high >= 0)
        RELEASE_AND_RETURN(scope, binasciiRaise(globalObject, "Odd number of hexadecimal digits"_s));
    RELEASE_AND_RETURN(scope, writer.finish(globalObject, out));
}

// ---- Quoted-printable ----

static JSValue binasciiA2BQP(JSGlobalObject* globalObject, JSValue dataValue, JSValue headerValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    BinasciiSource data;
    bool isHeader = false;
    if (failed(scope, data.setASCII(globalObject, dataValue)) || failed(scope, binasciiBoolArgument(globalObject, headerValue, isHeader))
        || !data.refresh())
        return { };
    auto input = data.bytes();
    size_t size = input.size();
    BinasciiWriter writer;
    if (!writer.allocate(globalObject, size))
        return { };
    uint8_t* out = writer.start();
    for (size_t i = 0; i < size;) {
        uint8_t character = input[i++];
        if (character == '_' && isHeader) {
            *out++ = ' ';
            continue;
        }
        if (character != '=') {
            *out++ = character;
            continue;
        }
        if (i >= size)
            break;
        if (input[i] == '\n' || input[i] == '\r') {
            // A soft line break: the rest of the line goes, with its end.
            while (i < size && input[i] != '\n')
                ++i;
            if (i < size)
                ++i;
        } else if (input[i] == '=') {
            // What an old encoder of Python's made of '='.
            *out++ = '=';
            ++i;
        } else if (i + 1 < size && binasciiHexDigit(input[i]) >= 0 && binasciiHexDigit(input[i + 1]) >= 0) {
            *out++ = static_cast<uint8_t>(binasciiHexDigit(input[i]) << 4 | binasciiHexDigit(input[i + 1]));
            i += 2;
        } else
            *out++ = '=';
    }
    RELEASE_AND_RETURN(scope, writer.finish(globalObject, out));
}

static JSValue binasciiB2AQP(JSGlobalObject* globalObject, JSValue dataValue, JSValue quoteTabsValue, JSValue isTextValue, JSValue headerValue)
{
    static constexpr char digits[] = "0123456789ABCDEF";
    static constexpr unsigned maxLineLength = 76;
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    BinasciiSource data;
    bool quoteTabs = false;
    bool isText = true;
    bool isHeader = false;
    if (failed(scope, data.setBuffer(globalObject, dataValue))
        || failed(scope, binasciiBoolArgument(globalObject, quoteTabsValue, quoteTabs))
        || failed(scope, binasciiBoolArgument(globalObject, isTextValue, isText))
        || failed(scope, binasciiBoolArgument(globalObject, headerValue, isHeader))
        || !data.refresh())
        return { };
    auto input = data.bytes();
    size_t size = input.size();

    // All the lines end as the first one does.
    bool usesCRLF = false;
    if (auto* newline = static_cast<const uint8_t*>(size ? memchr(input.data(), '\n', size) : nullptr))
        usesCRLF = newline > input.data() && newline[-1] == '\r';

    Vector<uint8_t, 256> output;
    if (!output.tryReserveCapacity(size + size / 8))
        return raiseMemoryError(globalObject, scope, String());
    unsigned lineLength = 0;
    auto appendLineEnd = [&] {
        if (usesCRLF)
            output.append('\r');
        output.append('\n');
        lineLength = 0;
    };
    auto appendEscaped = [&](uint8_t character) {
        output.append('=');
        output.append(digits[character >> 4]);
        output.append(digits[character & 15]);
    };
    for (size_t i = 0; i < size;) {
        uint8_t character = input[i];
        bool isLast = i + 1 == size;
        bool isBlank = character == ' ' || character == '\t';
        bool isLineBreakCharacter = character == '\r' || character == '\n';
        bool mustEscape = character > 126 || character == '='
            || (isHeader && character == '_')
            // A line that is a lone '.' ends a message in SMTP.
            || (character == '.' && !lineLength && (isLast || input[i + 1] == '\n' || input[i + 1] == '\r' || !input[i + 1]))
            || (!isText && isLineBreakCharacter)
            || (isBlank && isLast)
            || (character < 33 && !isLineBreakCharacter && (quoteTabs || !isBlank));
        if (mustEscape) {
            if (lineLength + 3 >= maxLineLength) {
                output.append('=');
                appendLineEnd();
            }
            appendEscaped(character);
            lineLength += 3;
            ++i;
            continue;
        }
        if (isText && (character == '\n' || (!isLast && character == '\r' && input[i + 1] == '\n'))) {
            // A blank at the end of a line would get lost.
            if (!output.isEmpty() && (output.last() == ' ' || output.last() == '\t'))
                appendEscaped(output.takeLast());
            appendLineEnd();
            i += character == '\r' ? 2 : 1;
            continue;
        }
        if (!isLast && input[i + 1] != '\n' && lineLength + 1 >= maxLineLength) {
            output.append('=');
            appendLineEnd();
        }
        ++lineLength;
        output.append(isHeader && character == ' ' ? static_cast<uint8_t>('_') : character);
        ++i;
    }
    RELEASE_AND_RETURN(scope, PyBytes::create(globalObject, output.span()));
}

// ---- The module ----

static constexpr NativeDef binasciiFunctions[] = {
    PY_FUNCTION("a2b_uu", binasciiA2BUU, "(data, /)"),
    PY_FUNCTION("b2a_uu", binasciiB2AUU, "(data, /, *, backtick=False)"),
    PY_FUNCTION("a2b_base64", binasciiA2BBase64, "(data, /, *, strict_mode=<unrepresentable>, padded=True, alphabet=BASE64_ALPHABET, ignorechars=<unrepresentable>, canonical=False)"),
    PY_FUNCTION("b2a_base64", binasciiB2ABase64, "(data, /, *, padded=True, wrapcol=0, newline=True, alphabet=BASE64_ALPHABET)"),
    PY_FUNCTION("b2a_ascii85", binasciiB2AAscii85, "(data, /, *, foldspaces=False, wrapcol=0, pad=False, adobe=False)"),
    PY_FUNCTION("a2b_ascii85", binasciiA2BAscii85, "(data, /, *, foldspaces=False, adobe=False, ignorechars=b'', canonical=False)"),
    PY_FUNCTION("a2b_base85", binasciiA2BBase85, "(data, /, *, alphabet=BASE85_ALPHABET, ignorechars=b'', canonical=False)"),
    PY_FUNCTION("b2a_base85", binasciiB2ABase85, "(data, /, *, pad=False, wrapcol=0, alphabet=BASE85_ALPHABET)"),
    PY_FUNCTION("a2b_base32", binasciiA2BBase32, "(data, /, *, padded=True, alphabet=BASE32_ALPHABET, ignorechars=b'', canonical=False)"),
    PY_FUNCTION("b2a_base32", binasciiB2ABase32, "(data, /, *, padded=True, wrapcol=0, alphabet=BASE32_ALPHABET)"),
    PY_FUNCTION("a2b_hex", binasciiA2BHex, "(hexstr, /, *, ignorechars=b'')"),
    PY_FUNCTION("b2a_hex", binasciiB2AHex, "(data, sep=<unrepresentable>, bytes_per_sep=1)"),
    PY_FUNCTION("hexlify", binasciiB2AHex, "(data, sep=<unrepresentable>, bytes_per_sep=1)"),
    PY_FUNCTION("unhexlify", binasciiA2BHex, "(hexstr, /, *, ignorechars=b'')"),
    PY_FUNCTION("crc_hqx", binasciiCRCHqx, "(data, crc, /)"),
    PY_FUNCTION("crc32", binasciiCRC32, "(data, crc=0, /)"),
    PY_FUNCTION("a2b_qp", binasciiA2BQP, "(data, header=False)"),
    PY_FUNCTION("b2a_qp", binasciiB2AQP, "(data, quotetabs=False, istext=True, header=False)"),
};

void initializeBinasciiModule(JSGlobalObject* globalObject, PyModule* module)
{
    Runtime& rt = runtime(globalObject);
    module->add(globalObject, "__doc__"_s, makeStr(rt.vm(), "Conversion between binary data and ASCII"_s));
    module->addFunctions(globalObject, binasciiFunctions);
    module->add(globalObject, "Error"_s, binasciiError(globalObject));
    module->add(globalObject, "Incomplete"_s, rt.typeFor(binasciiIncompleteTypeDef, rt.Exception()));

    auto addAlphabet = [&](ASCIILiteral name, auto characters) {
        module->add(globalObject, name, PyBytes::create(globalObject, characters.span()));
    };
    addAlphabet("BASE64_ALPHABET"_s, binasciiBase64Alphabet);
    addAlphabet("URLSAFE_BASE64_ALPHABET"_s, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"_s);
    addAlphabet("CRYPT_ALPHABET"_s, "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"_s);
    addAlphabet("UU_ALPHABET"_s, " !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_"_s);
    addAlphabet("BINHEX_ALPHABET"_s, "!\"#$%&'()*+,-012345689@ABCDEFGHIJKLMNPQRSTUVXYZ[`abcdefhijklmpqr"_s);
    addAlphabet("BASE85_ALPHABET"_s, binasciiBase85Alphabet);
    addAlphabet("ASCII85_ALPHABET"_s, binasciiAscii85Alphabet);
    addAlphabet("Z85_ALPHABET"_s, "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ.-:+=^!/*?&<>()[]{}@%$#"_s);
    addAlphabet("BASE32_ALPHABET"_s, binasciiBase32Alphabet);
    addAlphabet("BASE32HEX_ALPHABET"_s, "0123456789ABCDEFGHIJKLMNOPQRSTUV"_s);
}

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
