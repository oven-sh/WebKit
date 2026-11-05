/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyStr.h"

#include "JSCInlines.h"
#include "PyAbstract.h"
#include "PyBytes.h"
#include "PyErrors.h"
#include "PyInternals.h"
#include "PyIterators.h"
#include "PyList.h"
#include "PyModuleHelpers.h"
#include "PyNumbers.h"
#include "PyRange.h"
#include "PyTextSearch.h"
#include "PyTypes.h"
#include <unicode/uchar.h>
#include <unicode/ustring.h>
#include <wtf/HexNumber.h>
#include <wtf/SIMDUTF.h>
#include <wtf/text/StringBuilder.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {

// str. (Objects/unicodeobject.c in CPython is the reference.)

PY_DEFINE_CELL(PyStr);

PyStr::PyStr(VM& vm, Structure* structure, PyType* type, JSString* value)
    : Base(vm, structure, type)
    , m_value(value, WriteBarrierEarlyInit)
{
}

PyStr* PyStr::create(VM& vm, Structure* structure, PyType* type, JSString* value)
{
    auto* object = new (NotNull, allocateCell<PyStr>(vm)) PyStr(vm, structure, type, value);
    object->finishCreation(vm);
    return object;
}

template<typename Visitor>
void PyStr::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<PyStr>(cell);
    Base::visitChildren(thisObject, visitor);
    visitor.append(thisObject->m_value);
}

DEFINE_VISIT_CHILDREN(PyStr);

// ---- Making strings ----

JSString* internString(JSGlobalObject* globalObject, JSString* string)
{
    // The cell takes the text that is the one of its kind.
    string->toAtomString(globalObject);
    return string;
}

JSString* internString(VM& vm, const String& string)
{
    if (string.isEmpty())
        return jsEmptyString(vm);
    return jsString(vm, AtomString(string).string());
}

JSString* internString(VM& vm, ASCIILiteral literal)
{
    if (literal.isEmpty())
        return jsEmptyString(vm);
    return jsString(vm, String(AtomStringImpl::add(literal)));
}

JSString* internString(VM& vm, ASCIILiteral literal, unsigned hash)
{
    if (literal.isEmpty())
        return jsEmptyString(vm);
    ASSERT(hash == StringHasher::computeHashAndMaskTop8Bits(literal.span8()));
    WTF::HashTranslatorCharBuffer<Latin1Character> buffer { literal.span8(), hash };
    return jsString(vm, String(AtomStringImpl::addWithoutCopying(buffer)));
}

JSString* makeStr(VM& vm, const String& string)
{
    if (string.isEmpty())
        return jsEmptyString(vm);
    return jsString(vm, string);
}

JSString* makeStr(VM& vm, String&& string)
{
    return jsString(vm, WTF::move(string));
}

JSString* makeStr(VM& vm, ASCIILiteral literal)
{
    return makeStr(vm, String(literal));
}

JSString* partOfStr(VM& vm, const String& string, unsigned from, unsigned length)
{
    constexpr unsigned shortestThatShares = 256;
    StringImpl& whole = *string.impl();
    ASSERT(from <= whole.length() && length <= whole.length() - from);
    // A part of a part shares the text of the whole.
    unsigned lengthKeptAlive = whole.isSubString() ? whole.baseOfSubString().length() : whole.length();
    if (length <= 1 || (length >= shortestThatShares && length >= lengthKeptAlive / 2))
        return jsSubstring(vm, string, from, length);
    RefPtr<StringImpl> part;
    if (whole.is8Bit()) {
        std::span<Latin1Character> copy;
        if ((part = StringImpl::tryCreateUninitialized(length, copy)))
            StringImpl::copyCharacters(copy, whole.span8().subspan(from, length));
    } else if (auto units = whole.span16().subspan(from, length); WTF::charactersAreAllLatin1(units)) {
        std::span<Latin1Character> copy;
        if ((part = StringImpl::tryCreateUninitialized(length, copy)))
            StringImpl::copyCharacters(copy, units);
    } else {
        std::span<char16_t> copy;
        if ((part = StringImpl::tryCreateUninitialized(length, copy)))
            StringImpl::copyCharacters(copy, units);
    }
    if (!part) [[unlikely]]
        return jsSubstring(vm, string, from, length); // There is no room for a copy.
    return jsNontrivialString(vm, String(part.releaseNonNull()));
}

JSString* makeStrUnlessTooLong(JSGlobalObject* globalObject, String&& string)
{
    VM& vm = getVM(globalObject);
    if (string.isNull()) [[unlikely]] {
        auto scope = DECLARE_THROW_SCOPE(vm);
        raiseMemoryError(globalObject, scope, String());
        return nullptr;
    }
    return makeStr(vm, WTF::move(string));
}

JSString* makeStr(JSGlobalObject* globalObject, StrBuilder& builder)
{
    return makeStrUnlessTooLong(globalObject, builder.toStringOrNull());
}

void appendCodePoint(StringBuilder& builder, char32_t character)
{
    if (U_IS_BMP(character))
        builder.append(static_cast<char16_t>(character));
    else
        builder.append(U16_LEAD(character), U16_TRAIL(character));
}

void appendRepeated(StringBuilder& builder, char32_t character, int64_t count)
{
    if (count > String::MaxLength)
        builder.didOverflow();
    if (builder.hasOverflowed())
        return;
    for (int64_t i = 0; i < count; ++i)
        appendCodePoint(builder, character);
}

String stringFromCodePoint(char32_t character)
{
    if (character <= 0xff) {
        Latin1Character latin1 = static_cast<Latin1Character>(character);
        return String(std::span<const Latin1Character> { &latin1, 1 });
    }
    char16_t units[2];
    if (U_IS_BMP(character)) {
        units[0] = static_cast<char16_t>(character);
        return String(std::span<const char16_t> { units, 1 });
    }
    units[0] = U16_LEAD(character);
    units[1] = U16_TRAIL(character);
    return String(std::span<const char16_t> { units, 2 });
}

// ---- StrView ----

// Whether any unit is the second half of a pair: in most text none is.
static bool hasSecondHalves(std::span<const char16_t> units)
{
    auto mask = SIMD::splat<uint16_t>(0xfc00);
    auto secondHalf = SIMD::splat<uint16_t>(0xdc00);
    auto vectorMatch = [&](auto value) ALWAYS_INLINE_LAMBDA {
        return SIMD::findFirstNonZeroIndex(SIMD::equal(SIMD::bitAnd(value, mask), secondHalf));
    };
    auto scalarMatch = [&](char16_t unit) ALWAYS_INLINE_LAMBDA {
        return U16_IS_TRAIL(unit);
    };
    return SIMD::find(units, vectorMatch, scalarMatch) != std::to_address(units.end());
}

static unsigned countSurrogatePairs(std::span<const char16_t> units)
{
    unsigned pairs = 0;
    for (size_t i = 0; i + 1 < units.size(); ++i) {
        if (U16_IS_LEAD(units[i]) && U16_IS_TRAIL(units[i + 1])) {
            ++pairs;
            ++i;
        }
    }
    return pairs;
}

// Texts shorter than this are gone through each time.
static constexpr unsigned shortestStrRemembered = 48;

void StrView::lookThrough()
{
    auto units = string().span16();
    unsigned pairs = hasSecondHalves(units) ? countSurrogatePairs(units) : 0;
    m_length = units.size() - pairs;
    m_isSimple = !pairs;
    if (!pairs || units.size() < shortestStrRemembered)
        return;
    m_checkpoints = StrCheckpoints::create();
    auto& offsets = m_checkpoints->offsets;
    offsets.reserveInitialCapacity(m_length / StrCheckpoints::interval + 1);
    unsigned offset = 0;
    for (unsigned index = 0; index < m_length; ++index) {
        if (!(index % StrCheckpoints::interval))
            offsets.append(offset);
        offset += (U16_IS_LEAD(units[offset]) && offset + 1 < units.size() && U16_IS_TRAIL(units[offset + 1])) ? 2 : 1;
    }
}

StrView::StrView(const String& string)
    : m_text(nullptr, string.impl())
    , m_retained(string)
    , m_length(string.length())
{
    if (!string.is8Bit())
        lookThrough();
}

StringImpl* StrView::flatten(JSGlobalObject* globalObject, JSString* string)
{
    string->value(globalObject);
    // Null if that ran out of memory.
    return string->tryGetValueImpl();
}

struct StrIndexCache {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(StrIndexCache);
    struct Entry {
        RefPtr<StringImpl> impl; // Held, so that no other text can come to be at its address.
        unsigned length { 0 };
        RefPtr<StrCheckpoints> checkpoints;
    };
    static constexpr unsigned size = 16;
    std::array<Entry, size> entries;
    // The texts are let go of when the collector has run: it may have found that nothing else holds them.
    HeapVersion heapVersion { 0 };
};

void StrView::lookThroughOrRecall(JSGlobalObject* globalObject)
{
    if (m_length < shortestStrRemembered) {
        lookThrough();
        return;
    }
    Runtime& rt = runtime(globalObject);
    if (!rt.strIndexCache) [[unlikely]]
        rt.strIndexCache = { new StrIndexCache, [](StrIndexCache* cache) { delete cache; } };
    StrIndexCache& cache = *rt.strIndexCache;
    if (HeapVersion version = rt.vm().heap.objectSpace().markingVersion(); version != cache.heapVersion) [[unlikely]] {
        cache.entries = { };
        cache.heapVersion = version;
    }
    StringImpl* impl = m_text.data;
    auto& entry = cache.entries[PtrHash<StringImpl*>::hash(impl) % StrIndexCache::size];
    if (entry.impl == impl) [[likely]] {
        m_length = entry.length;
        m_isSimple = m_length == impl->length();
        m_checkpoints = entry.checkpoints;
        return;
    }
    lookThrough();
    entry = { impl, m_length, m_checkpoints };
}

unsigned StrView::unitOffsetSlow(unsigned index) const
{
    auto units = string().span16();
    unsigned offset = 0;
    if (m_checkpoints && index < m_length) {
        offset = m_checkpoints->offsets[index / StrCheckpoints::interval];
        index %= StrCheckpoints::interval;
    } else if (index >= m_length)
        return units.size();
    for (; index && offset < units.size(); --index)
        offset += (U16_IS_LEAD(units[offset]) && offset + 1 < units.size() && U16_IS_TRAIL(units[offset + 1])) ? 2 : 1;
    return offset;
}

unsigned StrView::indexOfUnitOffsetSlow(unsigned offset) const
{
    auto units = string().span16();
    if (!m_checkpoints)
        return offset - countSurrogatePairs(units.first(offset));
    auto& offsets = m_checkpoints->offsets;
    size_t checkpoint = std::ranges::upper_bound(offsets, offset) - offsets.begin() - 1;
    unsigned from = offsets[checkpoint];
    return checkpoint * StrCheckpoints::interval + (offset - from) - countSurrogatePairs(units.subspan(from, offset - from));
}

char32_t StrView::atSlow(unsigned index) const
{
    auto units = string().span16();
    unsigned offset = unitOffset(index);
    char16_t unit = units[offset];
    if (U16_IS_LEAD(unit) && offset + 1 < units.size() && U16_IS_TRAIL(units[offset + 1]))
        return U16_GET_SUPPLEMENTARY(unit, units[offset + 1]);
    return unit;
}

String StrView::substring(unsigned start, unsigned end) const
{
    unsigned from = unitOffset(start);
    unsigned to = unitOffset(end);
    return string().substring(from, to - from);
}

unsigned strLength(JSGlobalObject* globalObject, JSString* string)
{
    if (string->is8Bit())
        return string->length();
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    StrView view(globalObject, string);
    RETURN_IF_EXCEPTION(scope, 0);
    return view.length();
}

// ---- repr ----

static void appendEscaped(StringBuilder& builder, char32_t character)
{
    if (character <= 0xff)
        builder.append("\\x"_s, hex(character, 2, Lowercase));
    else if (character <= 0xffff)
        builder.append("\\u"_s, hex(character, 4, Lowercase));
    else
        builder.append("\\U"_s, hex(character, 8, Lowercase));
}

String strRepr(StringView text)
{
    bool hasSingle = false;
    bool hasDouble = false;
    for (unsigned i = 0; i < text.length(); ++i) {
        hasSingle |= text[i] == '\'';
        hasDouble |= text[i] == '"';
    }
    char quote = hasSingle && !hasDouble ? '"' : '\'';

    StrBuilder builder;
    builder.append(quote);
    for (char32_t character : text.codePoints()) {
        if (character == static_cast<char32_t>(quote) || character == '\\')
            builder.append('\\', static_cast<char>(character));
        else if (character == '\t')
            builder.append("\\t"_s);
        else if (character == '\n')
            builder.append("\\n"_s);
        else if (character == '\r')
            builder.append("\\r"_s);
        else if (character < ' ' || character == 0x7f)
            appendEscaped(builder, character);
        else if (character < 0x7f)
            builder.append(static_cast<char>(character));
        else if (isPrintable(character))
            appendCodePoint(builder, character);
        else
            appendEscaped(builder, character);
    }
    builder.append(quote);
    return builder.toStringOrNull();
}

String asciiEscape(StringView text)
{
    StrBuilder builder;
    for (char32_t character : text.codePoints()) {
        if (character < 0x80)
            builder.append(static_cast<char>(character));
        else
            appendEscaped(builder, character);
    }
    return builder.toStringOrNull();
}

// ---- UTF-8 ----

template<typename Bytes>
static void appendCharacterInUTF8(Bytes& result, char32_t character)
{
    if (character < 0x80)
        result.append(static_cast<uint8_t>(character));
    else if (character < 0x800) {
        result.append(static_cast<uint8_t>(0xc0 | (character >> 6)));
        result.append(static_cast<uint8_t>(0x80 | (character & 0x3f)));
    } else if (character < 0x10000) {
        result.append(static_cast<uint8_t>(0xe0 | (character >> 12)));
        result.append(static_cast<uint8_t>(0x80 | ((character >> 6) & 0x3f)));
        result.append(static_cast<uint8_t>(0x80 | (character & 0x3f)));
    } else {
        result.append(static_cast<uint8_t>(0xf0 | (character >> 18)));
        result.append(static_cast<uint8_t>(0x80 | ((character >> 12) & 0x3f)));
        result.append(static_cast<uint8_t>(0x80 | ((character >> 6) & 0x3f)));
        result.append(static_cast<uint8_t>(0x80 | (character & 0x3f)));
    }
}

std::optional<size_t> lengthInUTF8(StringView text)
{
    if (text.is8Bit()) {
        auto characters = byteCast<char>(text.span8());
        return simdutf::utf8_length_from_latin1(characters.data(), characters.size());
    }
    auto units = text.span16();
    if (!simdutf::validate_utf16(units.data(), units.size()))
        return std::nullopt;
    return simdutf::utf8_length_from_utf16(units.data(), units.size());
}

void writeUTF8(StringView text, std::span<uint8_t> result)
{
    size_t written;
    if (!text.is8Bit())
        written = simdutf::convert_valid_utf16_to_utf8(text.span16().data(), text.length(), byteCast<char>(result.data()));
    else if (result.size() == text.length()) {
        memcpySpan(result, text.span8());
        return;
    } else
        written = simdutf::convert_latin1_to_utf8(byteCast<char>(text.span8().data()), text.length(), byteCast<char>(result.data()));
    ASSERT_UNUSED(written, written == result.size());
}

std::optional<Vector<uint8_t>> tryEncodeUTF8(StringView text, bool allowSurrogates)
{
    Vector<uint8_t> result;
    if (auto length = lengthInUTF8(text)) {
        result.grow(*length);
        writeUTF8(text, result.mutableSpan());
        return result;
    }
    if (!allowSurrogates)
        return std::nullopt;
    result.reserveInitialCapacity(text.length());
    for (char32_t character : text.codePoints())
        appendCharacterInUTF8(result, character);
    return result;
}

void appendUTF8(ByteBuilder& result, char32_t character) { appendCharacterInUTF8(result, character); }

bool appendUTF8(ByteBuilder& result, StringView text, bool allowSurrogates)
{
    if (auto length = lengthInUTF8(text)) {
        if (auto room = result.appendUninitialized(*length); room.data())
            writeUTF8(text, room);
        return true;
    }
    if (!allowSurrogates)
        return false;
    for (char32_t character : text.codePoints())
        appendCharacterInUTF8(result, character);
    return true;
}

bool appendUTF8(JSGlobalObject* globalObject, ByteBuilder& result, JSString* string)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    String text = string->value(globalObject);
    RETURN_IF_EXCEPTION(scope, false);
    if (!appendUTF8(result, text)) {
        // Which raises.
        scope.release();
        strEncode(globalObject, string, nullptr, nullptr);
        return false;
    }
    if (result.hasOverflowed()) [[unlikely]] {
        raiseMemoryError(globalObject, scope, String());
        return false;
    }
    return true;
}

// Decodes one character. 0: the bytes at the start are not UTF-8.
static unsigned decodeOneUTF8(std::span<const uint8_t> bytes, char32_t& character)
{
    uint8_t first = bytes[0];
    if (first < 0x80) {
        character = first;
        return 1;
    }
    unsigned length = first >= 0xf0 ? 4 : first >= 0xe0 ? 3 : 2;
    if (first < 0xc2 || first > 0xf4 || bytes.size() < length)
        return 0;
    char32_t result = first & (0xff >> (length + 1));
    for (unsigned i = 1; i < length; ++i) {
        if ((bytes[i] & 0xc0) != 0x80)
            return 0;
        result = (result << 6) | (bytes[i] & 0x3f);
    }
    if ((length == 3 && (result < 0x800 || U_IS_SURROGATE(result))) || (length == 4 && (result < 0x10000 || result > 0x10ffff)))
        return 0;
    character = result;
    return length;
}

template<typename OnError>
static String decodeUTF8With(std::span<const uint8_t> bytes, const OnError& onError)
{
    bool isASCII = true;
    for (uint8_t byte : bytes)
        isASCII &= byte < 0x80;
    if (isASCII)
        return String(byteCast<Latin1Character>(bytes));
    StrBuilder builder;
    for (size_t i = 0; i < bytes.size();) {
        char32_t character;
        if (unsigned length = decodeOneUTF8(bytes.subspan(i), character)) {
            appendCodePoint(builder, character);
            i += length;
        } else
            builder.append(onError(bytes[i++]));
    }
    return builder.toStringOrNull();
}

String decodeUTF8Lenient(std::span<const uint8_t> bytes)
{
    return decodeUTF8With(bytes, [](uint8_t) -> char16_t { return 0xfffd; });
}

String decodeFilesystem(std::span<const uint8_t> bytes)
{
    return decodeUTF8With(bytes, [](uint8_t byte) -> char16_t { return 0xdc00 + byte; });
}

Vector<uint8_t> encodeFilesystem(StringView text)
{
    Vector<uint8_t> result;
    result.reserveInitialCapacity(text.length());
    for (char32_t character : text.codePoints()) {
        if (character >= 0xdc80 && character <= 0xdcff)
            result.append(static_cast<uint8_t>(character - 0xdc00));
        else
            appendCharacterInUTF8(result, character);
    }
    return result;
}

// ---- Helpers for the methods ----

// The character that begins at `index`, which goes on to the next one; and the one that ends there, to whose beginning it goes back.
static ALWAYS_INLINE char32_t nextCharacter(std::span<const Latin1Character> text, size_t& index) { return text[index++]; }
static ALWAYS_INLINE char32_t previousCharacter(std::span<const Latin1Character> text, size_t& index) { return text[--index]; }
static ALWAYS_INLINE char32_t nextCharacter(std::span<const char16_t> text, size_t& index)
{
    char32_t character = text[index++];
    if (U16_IS_LEAD(character) && index < text.size() && U16_IS_TRAIL(text[index])) [[unlikely]]
        character = U16_GET_SUPPLEMENTARY(character, text[index++]);
    return character;
}
static ALWAYS_INLINE char32_t previousCharacter(std::span<const char16_t> text, size_t& index)
{
    char32_t character = text[--index];
    if (U16_IS_TRAIL(character) && index && U16_IS_LEAD(text[index - 1])) [[unlikely]]
        character = U16_GET_SUPPLEMENTARY(text[--index], character);
    return character;
}

WideText::Result WideText::set(std::span<const char16_t> units)
{
    m_characters = { };
    if (!hasSecondHalves(units))
        return Result::NoPairs;
    m_memory = MallocSpan<char32_t>::tryMalloc(units.size() * sizeof(char32_t));
    if (!m_memory)
        return Result::NoRoom;
    auto characters = m_memory.mutableSpan();
    size_t size = 0;
    for (size_t index = 0; index < units.size();)
        characters[size++] = nextCharacter(units, index);
    // The second halves may all be on their own.
    if (size == units.size()) {
        m_memory = { };
        return Result::NoPairs;
    }
    m_characters = characters.first(size);
    return Result::Done;
}

#define PY_STR_METHOD_PROLOGUE(view) \
    VM& vm = getVM(globalObject); \
    auto scope = DECLARE_THROW_SCOPE(vm); \
    JSString* selfString = strValue(self); \
    UNUSED_VARIABLE(selfString); \
    StrView view(globalObject, selfString); \
    RETURN_IF_EXCEPTION(scope, { })

// False: a TypeError, "f() argument 1 must be str, not int".
static bool strArgument(JSGlobalObject* globalObject, JSValue value, ASCIILiteral function, int position, GCOwnedDataScope<StringView>& result)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!isStr(value)) {
        if (position)
            raiseTypeError(globalObject, scope, function, "() argument "_s, position, " must be str, not "_s, typeNameOfArgument(globalObject, value));
        else
            raiseTypeError(globalObject, scope, function, "() argument must be str, not "_s, typeNameOfArgument(globalObject, value));
        return false;
    }
    result = strValue(value)->view(globalObject);
    RETURN_IF_EXCEPTION(scope, false);
    return true;
}

static ALWAYS_INLINE JSString* sliceOfUnits(VM& vm, JSString* whole, const String& string, unsigned from, unsigned to)
{
    if (!from && to == string.length() && whole)
        return whole;
    return partOfStr(vm, string, from, to - from);
}

bool sliceBounds(JSGlobalObject* globalObject, JSValue start, JSValue end, int64_t length, int64_t& from, int64_t& to)
{
    if (!unpackSliceBounds(globalObject, start, end, from, to))
        return false;
    adjustSliceBounds(length, from, to);
    return true;
}

void adjustSliceBounds(int64_t length, int64_t& from, int64_t& to)
{
    if (to > length)
        to = length;
    else if (to < 0)
        to = std::max<int64_t>(to + length, 0);
    if (from < 0)
        from = std::max<int64_t>(from + length, 0);
}

bool unpackSliceBounds(JSGlobalObject* globalObject, JSValue start, JSValue end, int64_t& from, int64_t& to)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto convert = [&](JSValue value, int64_t& result) {
        if (!value || isNone(value))
            return true;
        if (!hasIndex(globalObject, value)) {
            raiseTypeError(globalObject, scope, "slice indices must be integers or have an __index__ method"_s);
            return false;
        }
        result = toSsize(globalObject, value, nullptr);
        return !scope.exception();
    };
    from = 0;
    to = std::numeric_limits<int64_t>::max();
    return convert(start, from) && convert(end, to);
}

bool normalizeIndex(JSGlobalObject* globalObject, JSValue index, int64_t length, ASCIILiteral message, int64_t& result)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t value = toSsize(globalObject, index, runtime(globalObject).IndexError());
    RETURN_IF_EXCEPTION(scope, false);
    if (value < 0)
        value += length;
    if (value < 0 || value >= length) {
        raiseIndexError(globalObject, scope, message);
        return false;
    }
    result = value;
    return true;
}

int compareStrings(StringView a, StringView b)
{
    unsigned common = std::min(a.length(), b.length());
    if (a.is8Bit() && b.is8Bit()) {
        if (int order = memcmp(a.span8().data(), b.span8().data(), common))
            return order;
        return a.length() < b.length() ? -1 : a.length() > b.length();
    }
    for (unsigned i = 0; i < common; ++i) {
        unsigned x = a[i];
        unsigned y = b[i];
        if (x == y)
            continue;
        // The code points they differ in. A surrogate may stand alone, and is then a code point below all that take a pair.
        if (i && U16_IS_LEAD(a[i - 1]) && U16_IS_TRAIL(x) != U16_IS_TRAIL(y))
            return U16_IS_TRAIL(x) ? 1 : -1;
        if (U16_IS_LEAD(x) && i + 1 < a.length() && U16_IS_TRAIL(a[i + 1]))
            x = U16_GET_SUPPLEMENTARY(x, a[i + 1]);
        if (U16_IS_LEAD(y) && i + 1 < b.length() && U16_IS_TRAIL(b[i + 1]))
            y = U16_GET_SUPPLEMENTARY(y, b[i + 1]);
        return x < y ? -1 : 1;
    }
    return a.length() < b.length() ? -1 : a.length() > b.length();
}

// ---- Basic protocol ----

static JSValue strReprMethod(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto view = strValue(self)->view(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, makeStrUnlessTooLong(globalObject, strRepr(view)));
}

static JSValue strStrMethod(JSGlobalObject*, JSValue self) { return strValue(self); }

static int64_t strHashMethod(JSGlobalObject* globalObject, JSValue self) { return hash(globalObject, strValue(self)); }

static JSValue strLenMethod(JSGlobalObject* globalObject, JSValue self) { return jsNumber(strLength(globalObject, strValue(self))); }

JSValue strCompare(JSGlobalObject* globalObject, JSString* a, JSString* b, CompareOperator op)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (op == CompareOperator::Eq || op == CompareOperator::NotEq) {
        bool equal = a->equal(globalObject, b);
        RETURN_IF_EXCEPTION(scope, { });
        return jsBoolean(equal == (op == CompareOperator::Eq));
    }
    auto viewA = a->view(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    auto viewB = b->view(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    int order = compareStrings(viewA, viewB);
    switch (op) {
    case CompareOperator::Lt:
        return jsBoolean(order < 0);
    case CompareOperator::LtE:
        return jsBoolean(order <= 0);
    case CompareOperator::Gt:
        return jsBoolean(order > 0);
    case CompareOperator::GtE:
        return jsBoolean(order >= 0);
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

template<CompareOperator op>
static JSValue strCompareMethod(JSGlobalObject* globalObject, JSValue self, JSValue other)
{
    if (!isStr(other))
        return runtime(globalObject).notImplemented();
    return strCompare(globalObject, strValue(self), strValue(other), op);
}

static JSValue strAdd(JSGlobalObject* globalObject, JSValue self, JSValue other)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!isStr(other))
        return raiseTypeError(globalObject, scope, "can only concatenate str (not \""_s, typeName(globalObject, other), "\") to str"_s);
    RELEASE_AND_RETURN(scope, jsString(globalObject, strValue(self), strValue(other)));
}

static String repeatString(const String& string, uint64_t count)
{
    if (count > String::MaxLength / string.length())
        return { };
    uint64_t total = string.length() * count;
    if (string.is8Bit()) {
        std::span<Latin1Character> buffer;
        String result = String::tryCreateUninitialized(total, buffer);
        if (result.isNull())
            return { };
        auto source = string.span8();
        if (source.size() == 1)
            memset(buffer.data(), source[0], total);
        else {
            for (uint64_t i = 0; i < count; ++i)
                memcpy(buffer.data() + i * source.size(), source.data(), source.size());
        }
        return result;
    }
    std::span<char16_t> buffer;
    String result = String::tryCreateUninitialized(total, buffer);
    if (result.isNull())
        return { };
    auto source = string.span16();
    for (uint64_t i = 0; i < count; ++i)
        memcpy(buffer.data() + i * source.size(), source.data(), source.size() * sizeof(char16_t));
    return result;
}

static JSValue strMul(JSGlobalObject* globalObject, JSValue self, JSValue countValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t count = toSsize(globalObject, countValue, runtime(globalObject).OverflowError());
    RETURN_IF_EXCEPTION(scope, { });
    JSString* string = strValue(self);
    if (count <= 0 || !string->length())
        return jsEmptyString(vm);
    if (count == 1)
        return string;
    String text = string->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    String result = repeatString(text, count);
    if (result.isNull()) {
        StrView view(globalObject, string);
        RETURN_IF_EXCEPTION(scope, { });
        if (static_cast<int64_t>(view.length()) > std::numeric_limits<int64_t>::max() / count)
            return raiseOverflowError(globalObject, scope, "repeated string is too long"_s);
        return raiseMemoryError(globalObject, scope, String());
    }
    return makeStr(vm, result);
}

static JSValue strMod(JSGlobalObject* globalObject, JSValue self, JSValue arguments)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue result = strFormatPercent(globalObject, strValue(self), arguments);
    RETURN_IF_EXCEPTION(scope, { });
    // Text with nothing to replace in it is the result as it is, be it an instance of a class derived from str.
    if (!self.isString() && strValue(self)->length()) {
        auto view = strValue(self)->view(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        if (view->find('%') == notFound)
            return self;
    }
    return result;
}

static JSValue strRMod(JSGlobalObject* globalObject, JSValue self, JSValue other)
{
    if (!isStr(other))
        return runtime(globalObject).notImplemented();
    return strFormatPercent(globalObject, strValue(other), self);
}

// What matches in UTF-16 matches in code points, unless it takes one half of a surrogate pair: a text that begins or ends
// with a lone surrogate can.
static ALWAYS_INLINE bool splitsSurrogatePair(StringView haystack, size_t position, size_t length)
{
    if (haystack.is8Bit())
        return false;
    if (position && position < haystack.length() && U16_IS_TRAIL(haystack[position]) && U16_IS_LEAD(haystack[position - 1]))
        return true;
    size_t end = position + length;
    return end && end < haystack.length() && U16_IS_LEAD(haystack[end - 1]) && U16_IS_TRAIL(haystack[end]);
}

template<bool backward>
static size_t findLongUnits(StringView haystack, StringView needle)
{
    if (haystack.is8Bit())
        return needle.is8Bit() ? twoWaySearch<backward>(haystack.span8(), needle.span8()) : twoWaySearch<backward>(haystack.span8(), needle.span16());
    return needle.is8Bit() ? twoWaySearch<backward>(haystack.span16(), needle.span8()) : twoWaySearch<backward>(haystack.span16(), needle.span16());
}

static size_t findUnits(StringView haystack, StringView needle, size_t start)
{
    if (needle.length() < shortestNeedleForTwoWaySearch || start > haystack.length()) [[likely]]
        return haystack.find(needle, start);
    size_t position = findLongUnits<false>(haystack.substring(start), needle);
    return position == notFound ? notFound : position + start;
}

// The last place that is not after `start`.
static size_t reverseFindUnits(StringView haystack, StringView needle, size_t start = std::numeric_limits<unsigned>::max())
{
    if (needle.length() < shortestNeedleForTwoWaySearch) [[likely]]
        return haystack.reverseFind(needle, start);
    return findLongUnits<true>(haystack.left(std::min<size_t>(haystack.length(), start + needle.length())), needle);
}

static size_t findText(StringView haystack, StringView needle, size_t start = 0)
{
    size_t position = findUnits(haystack, needle, start);
    while (position != notFound && splitsSurrogatePair(haystack, position, needle.length())) [[unlikely]]
        position = findUnits(haystack, needle, position + 1);
    return position;
}

static size_t reverseFindText(StringView haystack, StringView needle)
{
    size_t position = reverseFindUnits(haystack, needle);
    while (position != notFound && splitsSurrogatePair(haystack, position, needle.length())) [[unlikely]]
        position = position ? reverseFindUnits(haystack, needle, position - 1) : notFound;
    return position;
}

static bool startsWithText(StringView text, StringView prefix) { return text.startsWith(prefix) && !splitsSurrogatePair(text, 0, prefix.length()); }
static bool endsWithText(StringView text, StringView suffix) { return text.endsWith(suffix) && !splitsSurrogatePair(text, text.length() - suffix.length(), suffix.length()); }

bool strContains(JSGlobalObject* globalObject, JSString* string, JSValue item)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!isStr(item)) {
        raiseTypeError(globalObject, scope, "'in <string>' requires string as left operand, not "_s, typeName(globalObject, item));
        return false;
    }
    auto haystack = string->view(globalObject);
    RETURN_IF_EXCEPTION(scope, false);
    auto needle = strValue(item)->view(globalObject);
    RETURN_IF_EXCEPTION(scope, false);
    return findText(haystack, needle) != notFound;
}

static JSValue strContainsMethod(JSGlobalObject* globalObject, JSValue self, JSValue item)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool found = strContains(globalObject, strValue(self), item);
    RETURN_IF_EXCEPTION(scope, { });
    return jsBoolean(found);
}

JSValue strGetItem(JSGlobalObject* globalObject, JSValue self, JSValue key)
{
    PY_STR_METHOD_PROLOGUE(view);
    if (hasIndex(globalObject, key)) {
        int64_t index;
        if (failed(scope, normalizeIndex(globalObject, key, view.length(), "string index out of range"_s, index)))
            return { };
        if (view.isSimple())
            return jsSingleCharacterString(vm, static_cast<char16_t>(view.string()[index]));
        return makeStr(vm, stringFromCodePoint(view.at(index)));
    }
    auto* slice = PySlice::tryFrom(key);
    if (!slice)
        return raiseTypeError(globalObject, scope, "string indices must be integers, not '"_s, typeName(globalObject, key), '\'');
    PySlice::Indices indices;
    if (failed(scope, slice->indices(globalObject, view.length(), indices)))
        return { };
    if (indices.length <= 0)
        return jsEmptyString(vm);
    if (indices.step == 1)
        return sliceOfUnits(vm, selfString, view.string(), view.unitOffset(indices.start), view.unitOffset(indices.stop));
    StrBuilder builder;
    if (view.isSimple()) {
        int64_t position = indices.start;
        for (int64_t i = 0; i < indices.length; ++i, position += indices.step)
            builder.append(view.string()[position]);
    } else {
        auto units = view.string().span16();
        size_t position = view.unitOffset(indices.start);
        for (int64_t i = 0; i < indices.length; ++i) {
            size_t next = position;
            appendCodePoint(builder, nextCharacter(units, next));
            if (i + 1 == indices.length)
                break;
            if (indices.step > 0) {
                position = next;
                for (int64_t j = 1; j < indices.step; ++j)
                    nextCharacter(units, position);
            } else {
                for (int64_t j = 0; j > indices.step; --j)
                    previousCharacter(units, position);
            }
        }
    }
    RELEASE_AND_RETURN(scope, makeStr(globalObject, builder));
}

static JSValue strIter(JSGlobalObject* globalObject, JSValue self)
{
    PyIterator* iterator = PyIterator::create(globalObject, PyIterator::Kind::Str, strValue(self));
    // The iterator for ASCII is for strs laid out as str itself lays them out, in CPython.
    if (!self.isString()) [[unlikely]]
        iterator->setType(getVM(globalObject), runtime(globalObject).strIteratorType());
    return iterator;
}

static JSValue strGetNewArgs(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    // A copy: see isIdentical().
    String text = strValue(self)->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    if (text.length() < 2)
        RELEASE_AND_RETURN(scope, PyTuple::create(globalObject, { JSValue(strValue(self)) }));
    RELEASE_AND_RETURN(scope, PyTuple::create(globalObject, { JSValue(jsNontrivialString(vm, text.is8Bit() ? String(text.span8()) : String(text.span16()))) }));
}

static JSValue strFormatSpecMethod(JSGlobalObject* globalObject, JSValue self, JSValue spec)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!isStr(spec))
        return raiseTypeError(globalObject, scope, "__format__() argument must be str, not "_s, typeNameOfArgument(globalObject, spec));
    // A class derived from str may have a __str__ of its own.
    if (!strValue(spec)->length())
        RELEASE_AND_RETURN(scope, isExactStr(self) ? asString(self) : str(globalObject, self));
    JSString* result = formatStr(globalObject, self, strValue(spec));
    RETURN_IF_EXCEPTION(scope, { });
    // With nothing to add or to cut off it is the object itself, of whatever class, unless it is empty.
    return result == strValue(self) && result->length() ? self : JSValue(result);
}

// What the text takes in CPython, which stores it in units as wide as its widest character needs.
static JSValue strSizeOf(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto view = strValue(self)->view(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    uint64_t length = 0;
    char32_t widest = 0;
    for (char32_t character : view->codePoints()) {
        widest = std::max(widest, character);
        ++length;
    }
    if (widest < 0x80)
        return jsNumber(static_cast<double>(40 + length + 1));
    return jsNumber(static_cast<double>(56 + (length + 1) * (widest < 0x100 ? 1 : widest < 0x10000 ? 2 : 4)));
}

// ---- Searching ----

// In code points. -1: not found.
static int64_t findIn(const StrView& view, StringView needle, int64_t from, int64_t to, bool reverse)
{
    if (from > to)
        return -1;
    if (needle.isEmpty())
        return reverse ? to : from;
    unsigned start = view.unitOffset(from);
    unsigned end = view.unitOffset(to);
    StringView haystack = view.view().substring(start, end - start);
    size_t position = reverse ? reverseFindText(haystack, needle) : findText(haystack, needle);
    if (position == notFound)
        return -1;
    return view.indexOfUnitOffset(start + position);
}

template<bool reverse, bool mustFind>
static JSValue strFindMethod(JSGlobalObject* globalObject, JSValue self, JSValue sub, JSValue start, JSValue end)
{
    PY_STR_METHOD_PROLOGUE(view);
    GCOwnedDataScope<StringView> needle;
    if (failed(scope, strArgument(globalObject, sub, mustFind ? (reverse ? "rindex"_s : "index"_s) : (reverse ? "rfind"_s : "find"_s), 1, needle)))
        return { };
    int64_t from, to;
    if (failed(scope, sliceBounds(globalObject, start, end, view.length(), from, to)))
        return { };
    int64_t result = findIn(view, needle, from, to, reverse);
    if (mustFind && result < 0)
        return raiseValueError(globalObject, scope, "substring not found"_s);
    return jsNumber(static_cast<double>(result));
}

static JSValue strCount(JSGlobalObject* globalObject, JSValue self, JSValue sub, JSValue start, JSValue end)
{
    PY_STR_METHOD_PROLOGUE(view);
    GCOwnedDataScope<StringView> needleText;
    if (failed(scope, strArgument(globalObject, sub, "count"_s, 1, needleText)))
        return { };
    StringView needle = needleText;
    int64_t from, to;
    if (failed(scope, sliceBounds(globalObject, start, end, view.length(), from, to)))
        return { };
    if (from > to)
        return jsNumber(0);
    if (needle.isEmpty())
        return jsNumber(static_cast<double>(to - from + 1));
    unsigned startUnit = view.unitOffset(from);
    unsigned endUnit = view.unitOffset(to);
    StringView haystack = view.view().substring(startUnit, endUnit - startUnit);
    unsigned count = 0;
    for (size_t position = 0; (position = findText(haystack, needle, position)) != notFound; position += needle.length())
        ++count;
    return jsNumber(count);
}

template<bool atEnd>
static JSValue strStartsOrEndsWith(JSGlobalObject* globalObject, JSValue self, JSValue affix, JSValue start, JSValue end)
{
    PY_STR_METHOD_PROLOGUE(view);
    int64_t from, to;
    if (failed(scope, sliceBounds(globalObject, start, end, view.length(), from, to)))
        return { };
    auto matches = [&](JSValue candidate) -> bool {
        auto text = strValue(candidate)->view(globalObject);
        RETURN_IF_EXCEPTION(scope, false);
        if (from > to)
            return false;
        unsigned startUnit = view.unitOffset(from);
        unsigned endUnit = view.unitOffset(to);
        StringView window = view.view().substring(startUnit, endUnit - startUnit);
        return atEnd ? endsWithText(window, text) : startsWithText(window, text);
    };
    if (auto* tuple = PyTuple::tryFrom(affix)) {
        for (unsigned i = 0; i < tuple->length(); ++i) {
            if (!isStr(tuple->at(i)))
                return raiseTypeError(globalObject, scope, "tuple for "_s, atEnd ? "endswith"_s : "startswith"_s, " must only contain str, not "_s, typeName(globalObject, tuple->at(i)));
            bool result = matches(tuple->at(i));
            RETURN_IF_EXCEPTION(scope, { });
            if (result)
                return jsBoolean(true);
        }
        return jsBoolean(false);
    }
    if (!isStr(affix))
        return raiseTypeError(globalObject, scope, atEnd ? "endswith"_s : "startswith"_s, " first arg must be str or a tuple of str, not "_s, typeName(globalObject, affix));
    bool result = matches(affix);
    RETURN_IF_EXCEPTION(scope, { });
    return jsBoolean(result);
}

static JSValue strRemovePrefix(JSGlobalObject* globalObject, JSValue self, JSValue prefixValue)
{
    PY_STR_METHOD_PROLOGUE(view);
    GCOwnedDataScope<StringView> prefix;
    if (failed(scope, strArgument(globalObject, prefixValue, "removeprefix"_s, 0, prefix)))
        return { };
    if (startsWithText(view.string(), prefix))
        return sliceOfUnits(vm, selfString, view.string(), prefix->length(), view.unitLength());
    return selfString;
}

static JSValue strRemoveSuffix(JSGlobalObject* globalObject, JSValue self, JSValue suffixValue)
{
    PY_STR_METHOD_PROLOGUE(view);
    GCOwnedDataScope<StringView> suffix;
    if (failed(scope, strArgument(globalObject, suffixValue, "removesuffix"_s, 0, suffix)))
        return { };
    if (endsWithText(view.string(), suffix))
        return sliceOfUnits(vm, selfString, view.string(), 0, view.unitLength() - suffix->length());
    return selfString;
}

static JSValue strReplace(JSGlobalObject* globalObject, JSValue self, JSValue oldValue, JSValue newValue, JSValue countValue)
{
    PY_STR_METHOD_PROLOGUE(view);
    GCOwnedDataScope<StringView> targetText, replacementText;
    if (failed(scope, strArgument(globalObject, oldValue, "replace"_s, 1, targetText)) || failed(scope, strArgument(globalObject, newValue, "replace"_s, 2, replacementText)))
        return { };
    StringView target = targetText;
    StringView replacement = replacementText;
    int64_t count = -1;
    if (countValue && failed(scope, ssizeArgument(globalObject, countValue, count)))
        return { };
    if (count < 0)
        count = std::numeric_limits<int64_t>::max();
    if (!count)
        return selfString;

    const String& string = view.string();
    StrBuilder builder;
    if (target.isEmpty()) {
        // Between all the characters, and at both ends.
        builder.append(replacement);
        --count;
        bool first = true;
        for (char32_t character : view.view().codePoints()) {
            if (!first && count > 0) {
                builder.append(replacement);
                --count;
            }
            first = false;
            appendCodePoint(builder, character);
        }
        if (!first && count > 0)
            builder.append(replacement);
        RELEASE_AND_RETURN(scope, makeStr(globalObject, builder));
    }
    size_t position = findText(string, target);
    if (position == notFound)
        return selfString;
    unsigned copied = 0;
    while (position != notFound && count > 0) {
        builder.append(StringView(string).substring(copied, position - copied), replacement);
        copied = position + target.length();
        --count;
        position = findText(string, target, copied);
    }
    builder.append(StringView(string).substring(copied));
    if (builder.hasOverflowed())
        return raiseOverflowError(globalObject, scope, "replace string is too long"_s);
    RELEASE_AND_RETURN(scope, makeStr(globalObject, builder));
}

// ---- Splitting and joining ----

static bool parseMaxSplit(JSGlobalObject* globalObject, JSValue value, int64_t& result)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    result = std::numeric_limits<int64_t>::max();
    if (!value)
        return true;
    int64_t given;
    if (failed(scope, ssizeArgument(globalObject, value, given)))
        return false;
    if (given >= 0)
        result = given;
    return true;
}

// Whether CPython sees without searching that the separator is not in the text: it is longer, or has characters that take more
// bytes (1, 2 or 4) than any in the text. The one item of the result is then the object itself, of whatever class derived from str.
static bool cannotHold(const StrView& view, StringView separator)
{
    auto measure = [](StringView string, int64_t& length) {
        length = string.length();
        if (string.is8Bit())
            return 1;
        int width = 1;
        auto units = string.span16();
        for (size_t i = 0; i < units.size(); ++i) {
            if (units[i] > 0xff)
                width = std::max(width, 2);
            if (U16_IS_LEAD(units[i]) && i + 1 < units.size() && U16_IS_TRAIL(units[i + 1])) {
                width = 4;
                --length;
                ++i;
            }
        }
        return width;
    };
    int64_t length, separatorLength;
    int width = measure(view.string(), length);
    return width < measure(separator, separatorLength) || length < separatorLength;
}

static JSValue strSplit(JSGlobalObject* globalObject, JSValue self, JSValue separatorValue, JSValue maxSplitValue)
{
    PY_STR_METHOD_PROLOGUE(view);
    int64_t maxSplit;
    if (failed(scope, parseMaxSplit(globalObject, maxSplitValue, maxSplit)))
        return { };
    const String& string = view.string();
    unsigned length = string.length();
    JSArray* result = newList(globalObject);
    RETURN_IF_EXCEPTION(scope, { });

    if (!separatorValue || isNone(separatorValue)) {
        unsigned i = 0;
        while (true) {
            while (i < length && isPythonWhitespace(string[i]))
                ++i;
            if (i == length)
                break;
            if (!maxSplit--) {
                // The rest, whitespace at its end and all.
                listAppend(globalObject, result, sliceOfUnits(vm, nullptr, string, i, length));
                RETURN_IF_EXCEPTION(scope, { });
                break;
            }
            unsigned start = i;
            while (i < length && !isPythonWhitespace(string[i]))
                ++i;
            listAppend(globalObject, result, sliceOfUnits(vm, selfString, string, start, i));
            RETURN_IF_EXCEPTION(scope, { });
        }
        return result;
    }
    if (!isStr(separatorValue))
        return raiseTypeError(globalObject, scope, "must be str or None, not "_s, typeName(globalObject, separatorValue));
    auto separatorText = strValue(separatorValue)->view(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    StringView separator = separatorText;
    if (separator.isEmpty())
        return raiseValueError(globalObject, scope, "empty separator"_s);
    if (!self.isString() && cannotHold(view, separator)) [[unlikely]] {
        scope.release();
        listAppend(globalObject, result, self);
        return result;
    }
    unsigned start = 0;
    while (maxSplit-- > 0) {
        size_t position = findText(string, separator, start);
        if (position == notFound)
            break;
        listAppend(globalObject, result, sliceOfUnits(vm, nullptr, string, start, position));
        RETURN_IF_EXCEPTION(scope, { });
        start = position + separator.length();
    }
    listAppend(globalObject, result, sliceOfUnits(vm, selfString, string, start, length));
    RETURN_IF_EXCEPTION(scope, { });
    return result;
}

static void reverseList(JSGlobalObject* globalObject, JSArray* list)
{
    unsigned length = listLength(list);
    for (unsigned i = 0; i < length / 2; ++i) {
        JSValue first = listGet(list, i);
        listSet(globalObject, list, i, listGet(list, length - 1 - i));
        listSet(globalObject, list, length - 1 - i, first);
    }
}

static JSValue strRSplit(JSGlobalObject* globalObject, JSValue self, JSValue separatorValue, JSValue maxSplitValue)
{
    PY_STR_METHOD_PROLOGUE(view);
    int64_t maxSplit;
    if (failed(scope, parseMaxSplit(globalObject, maxSplitValue, maxSplit)))
        return { };
    const String& string = view.string();
    JSArray* result = newList(globalObject);
    RETURN_IF_EXCEPTION(scope, { });

    if (!separatorValue || isNone(separatorValue)) {
        unsigned i = string.length();
        while (true) {
            while (i && isPythonWhitespace(string[i - 1]))
                --i;
            if (!i)
                break;
            if (!maxSplit--) {
                listAppend(globalObject, result, sliceOfUnits(vm, nullptr, string, 0, i));
                RETURN_IF_EXCEPTION(scope, { });
                break;
            }
            unsigned end = i;
            while (i && !isPythonWhitespace(string[i - 1]))
                --i;
            listAppend(globalObject, result, sliceOfUnits(vm, selfString, string, i, end));
            RETURN_IF_EXCEPTION(scope, { });
        }
        reverseList(globalObject, result);
        return result;
    }
    if (!isStr(separatorValue))
        return raiseTypeError(globalObject, scope, "must be str or None, not "_s, typeName(globalObject, separatorValue));
    auto separatorText = strValue(separatorValue)->view(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    StringView separator = separatorText;
    if (separator.isEmpty())
        return raiseValueError(globalObject, scope, "empty separator"_s);
    if (!self.isString() && cannotHold(view, separator)) [[unlikely]] {
        scope.release();
        listAppend(globalObject, result, self);
        return result;
    }
    unsigned end = string.length();
    while (maxSplit-- > 0 && end >= separator.length()) {
        size_t position = reverseFindText(StringView(string).left(end), separator);
        if (position == notFound)
            break;
        listAppend(globalObject, result, sliceOfUnits(vm, nullptr, string, position + separator.length(), end));
        RETURN_IF_EXCEPTION(scope, { });
        end = position;
    }
    listAppend(globalObject, result, sliceOfUnits(vm, selfString, string, 0, end));
    RETURN_IF_EXCEPTION(scope, { });
    reverseList(globalObject, result);
    return result;
}

static JSValue strSplitLines(JSGlobalObject* globalObject, JSValue self, JSValue keepEndsValue)
{
    PY_STR_METHOD_PROLOGUE(view);
    bool keepEnds = false;
    if (keepEndsValue) {
        keepEnds = isTrue(globalObject, keepEndsValue);
        RETURN_IF_EXCEPTION(scope, { });
    }
    const String& string = view.string();
    unsigned length = string.length();
    JSArray* result = newList(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    for (unsigned i = 0; i < length;) {
        unsigned start = i;
        while (i < length && !Unicode::isLineBreak(string[i]))
            ++i;
        unsigned end = i;
        if (i < length) {
            i += string[i] == '\r' && i + 1 < length && string[i + 1] == '\n' ? 2 : 1;
            if (keepEnds)
                end = i;
        }
        listAppend(globalObject, result, sliceOfUnits(vm, selfString, string, start, end));
        RETURN_IF_EXCEPTION(scope, { });
    }
    return result;
}

template<bool reverse>
static JSValue strPartition(JSGlobalObject* globalObject, JSValue self, JSValue separatorValue)
{
    PY_STR_METHOD_PROLOGUE(view);
    if (!isStr(separatorValue))
        return raiseTypeError(globalObject, scope, "must be str, not "_s, typeNameForMessage(globalObject, typeOf(globalObject, separatorValue)));
    JSString* separatorString = strValue(separatorValue);
    auto separatorText = separatorString->view(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    StringView separator = separatorText;
    if (separator.isEmpty())
        return raiseValueError(globalObject, scope, "empty separator"_s);
    const String& string = view.string();
    size_t position = reverse ? reverseFindText(string, separator) : findText(string, separator);
    JSString* empty = jsEmptyString(vm);
    // The str and the separator are the objects themselves, of whatever class derived from str.
    if (position == notFound) {
        if (reverse)
            RELEASE_AND_RETURN(scope, PyTuple::create(globalObject, { empty, empty, self }));
        RELEASE_AND_RETURN(scope, PyTuple::create(globalObject, { self, empty, empty }));
    }
    RELEASE_AND_RETURN(scope, PyTuple::create(globalObject, { sliceOfUnits(vm, nullptr, string, 0, position), separatorValue, sliceOfUnits(vm, nullptr, string, position + separator.length(), string.length()) }));
}

static JSString* joinItems(JSGlobalObject* globalObject, JSString* separatorString, std::span<const JSValue> items)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    size_t count = items.size();
    if (!count)
        return jsEmptyString(vm);

    uint64_t total = static_cast<uint64_t>(separatorString->length()) * (count - 1);
    bool is8Bit = count == 1 || separatorString->is8Bit();
    bool hasRope = separatorString->isRope();
    for (size_t i = 0; i < count; ++i) {
        JSValue item = items[i];
        if (!isStr(item)) [[unlikely]] {
            // A hole, which only JS can make, reads as None.
            raiseTypeError(globalObject, scope, "sequence item "_s, i, ": expected str instance, "_s, typeName(globalObject, item ? item : jsUndefined()), " found"_s);
            return nullptr;
        }
        JSString* string = strValue(item);
        total += string->length();
        is8Bit &= string->is8Bit();
        hasRope |= string->isRope();
    }
    if (count == 1 && items[0].isString())
        return asString(items[0]);
    if (total > String::MaxLength) [[unlikely]] {
        raiseOverflowError(globalObject, scope, "join() result is too long for a Python string"_s);
        return nullptr;
    }
    if (!total)
        return jsEmptyString(vm);

    // Up to here, and from here on if all the texts are flat, nothing is allocated in the heap, so nothing else runs and `items`
    // may be the storage of a list. To flatten a rope does allocate: the strs are first put where nobody else gets at them. Their
    // lengths stay what they are.
    MarkedArgumentBuffer kept;
    if (hasRope) [[unlikely]] {
        for (JSValue item : items)
            kept.append(strValue(item));
        if (kept.hasOverflowed()) [[unlikely]] {
            raiseMemoryError(globalObject, scope, String());
            return nullptr;
        }
        items = spanOf(kept);
        for (JSValue item : items) {
            asString(item)->value(globalObject);
            RETURN_IF_EXCEPTION(scope, nullptr);
        }
        separatorString->value(globalObject);
        RETURN_IF_EXCEPTION(scope, nullptr);
    }

    StringView separator = separatorString->tryGetValueImpl();
    auto fill = [&]<typename CharacterType>(std::span<CharacterType> buffer) {
        for (size_t i = 0; i < count; ++i) {
            if (i && separator.length() == 1) {
                buffer[0] = static_cast<CharacterType>(separator[0]);
                skip(buffer, 1);
            } else if (i) {
                separator.getCharacters(buffer);
                skip(buffer, separator.length());
            }
            StringView text = strValue(items[i])->tryGetValueImpl();
            text.getCharacters(buffer);
            skip(buffer, text.length());
        }
        ASSERT(buffer.empty());
    };
    RefPtr<StringImpl> result;
    if (is8Bit) {
        std::span<Latin1Character> buffer;
        if ((result = StringImpl::tryCreateUninitialized(total, buffer))) [[likely]]
            fill(buffer);
    } else {
        std::span<char16_t> buffer;
        if ((result = StringImpl::tryCreateUninitialized(total, buffer))) [[likely]]
            fill(buffer);
    }
    RELEASE_AND_RETURN(scope, makeStrUnlessTooLong(globalObject, String(WTF::move(result))));
}

JSString* strJoin(JSGlobalObject* globalObject, JSString* separatorString, JSValue iterable)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);

    AllItems items(globalObject, iterable, "can only join an iterable"_s);
    RETURN_IF_EXCEPTION(scope, nullptr);
    RELEASE_AND_RETURN(scope, joinItems(globalObject, separatorString, items.span()));
}

static JSValue strJoinMethod(JSGlobalObject* globalObject, JSValue self, JSValue iterable)
{
    return strJoin(globalObject, strValue(self), iterable);
}

// ---- Stripping and padding ----

enum class StripSide : uint8_t { Left, Right, Both };

template<StripSide side>
static JSValue strStrip(JSGlobalObject* globalObject, JSValue self, JSValue charactersValue)
{
    PY_STR_METHOD_PROLOGUE(view);
    const String& string = view.string();
    unsigned start = 0;
    unsigned end = string.length();
    if (!charactersValue || isNone(charactersValue)) {
        if (side != StripSide::Right) {
            while (start < end && isPythonWhitespace(string[start]))
                ++start;
        }
        if (side != StripSide::Left) {
            while (end > start && isPythonWhitespace(string[end - 1]))
                --end;
        }
        return sliceOfUnits(vm, selfString, string, start, end);
    }
    if (!isStr(charactersValue))
        return raiseTypeError(globalObject, scope, side == StripSide::Left ? "lstrip"_s : side == StripSide::Right ? "rstrip"_s : "strip"_s, " arg must be None or str"_s);
    StrView characters(globalObject, strValue(charactersValue));
    RETURN_IF_EXCEPTION(scope, { });

    if (view.isSimple() && characters.isSimple()) {
        auto contains = [&](char16_t character) { return characters.string().find(character) != notFound; };
        if (side != StripSide::Right) {
            while (start < end && contains(string[start]))
                ++start;
        }
        if (side != StripSide::Left) {
            while (end > start && contains(string[end - 1]))
                --end;
        }
        return sliceOfUnits(vm, selfString, string, start, end);
    }
    auto contains = [&](char32_t character) {
        for (char32_t candidate : characters.view().codePoints()) {
            if (candidate == character)
                return true;
        }
        return false;
    };
    auto strip = [&](auto text) {
        size_t first = 0;
        size_t last = text.size();
        if (side != StripSide::Right) {
            for (size_t next = first; first < last && contains(nextCharacter(text, next)); first = next) { }
        }
        if (side != StripSide::Left) {
            for (size_t previous = last; last > first && contains(previousCharacter(text, previous)); last = previous) { }
        }
        return sliceOfUnits(vm, selfString, string, first, last);
    };
    return string.is8Bit() ? strip(string.span8()) : strip(string.span16());
}

static bool fillCharacter(JSGlobalObject* globalObject, JSValue value, char32_t& result)
{
    result = ' ';
    if (!value)
        return true;
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!isStr(value)) {
        raiseTypeError(globalObject, scope, "The fill character must be a unicode character, not "_s, typeName(globalObject, value));
        return false;
    }
    StrView view(globalObject, strValue(value));
    RETURN_IF_EXCEPTION(scope, false);
    if (view.length() != 1) {
        raiseTypeError(globalObject, scope, "The fill character must be exactly one character long"_s);
        return false;
    }
    result = view.at(0);
    return true;
}

static JSValue pad(JSGlobalObject* globalObject, JSString* string, const StrView& view, int64_t left, int64_t right, char32_t fill)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (left <= 0 && right <= 0)
        return string;
    left = std::max<int64_t>(left, 0);
    right = std::max<int64_t>(right, 0);
    int64_t unitsPerFill = U16_LENGTH(fill);
    if (left > String::MaxLength || right > String::MaxLength || (left + right) * unitsPerFill + view.unitLength() > String::MaxLength)
        return raiseMemoryError(globalObject, scope, String());
    StrBuilder builder;
    for (int64_t i = 0; i < left; ++i)
        appendCodePoint(builder, fill);
    builder.append(view.string());
    for (int64_t i = 0; i < right; ++i)
        appendCodePoint(builder, fill);
    RELEASE_AND_RETURN(scope, makeStr(globalObject, builder));
}

enum class Justify : uint8_t { Left, Right, Center };

template<Justify justify>
static JSValue strJustify(JSGlobalObject* globalObject, JSValue self, JSValue widthValue, JSValue fillValue)
{
    PY_STR_METHOD_PROLOGUE(view);
    int64_t width;
    if (failed(scope, ssizeArgument(globalObject, widthValue, width)))
        return { };
    char32_t fill;
    if (failed(scope, fillCharacter(globalObject, fillValue, fill)))
        return { };
    if (width <= static_cast<int64_t>(view.length()))
        return selfString;
    int64_t margin = width - view.length();
    scope.release();
    if (justify == Justify::Left)
        RELEASE_AND_RETURN(scope, pad(globalObject, selfString, view, 0, margin, fill));
    if (justify == Justify::Right)
        RELEASE_AND_RETURN(scope, pad(globalObject, selfString, view, margin, 0, fill));
    int64_t left = margin / 2 + (margin & width & 1);
    RELEASE_AND_RETURN(scope, pad(globalObject, selfString, view, left, margin - left, fill));
}

static JSValue strZFill(JSGlobalObject* globalObject, JSValue self, JSValue widthValue)
{
    PY_STR_METHOD_PROLOGUE(view);
    int64_t width;
    if (failed(scope, ssizeArgument(globalObject, widthValue, width)))
        return { };
    if (width <= static_cast<int64_t>(view.length()))
        return selfString;
    int64_t fill = width - view.length();
    if (width > String::MaxLength || fill + view.unitLength() > String::MaxLength)
        return raiseMemoryError(globalObject, scope, String());
    const String& string = view.string();
    StrBuilder builder;
    unsigned start = 0;
    if (string.length() && (string[0] == '+' || string[0] == '-')) {
        builder.append(string[0]);
        start = 1;
    }
    for (int64_t i = 0; i < fill; ++i)
        builder.append('0');
    builder.append(StringView(string).substring(start));
    RELEASE_AND_RETURN(scope, makeStr(globalObject, builder));
}

static JSValue strExpandTabs(JSGlobalObject* globalObject, JSValue self, JSValue tabSizeValue)
{
    PY_STR_METHOD_PROLOGUE(view);
    int64_t tabSize = 8;
    if (tabSizeValue) {
        tabSize = toCInt(globalObject, tabSizeValue);
        RETURN_IF_EXCEPTION(scope, { });
    }
    if (view.string().find('\t') == notFound)
        return selfString;
    StrBuilder builder;
    int64_t column = 0;
    for (char32_t character : view.view().codePoints()) {
        if (character == '\t') {
            if (tabSize > 0) {
                int64_t spaces = tabSize - column % tabSize;
                column += spaces;
                appendRepeated(builder, ' ', spaces);
            }
            continue;
        }
        ++column;
        appendCodePoint(builder, character);
        if (character == '\n' || character == '\r')
            column = 0;
    }
    if (builder.hasOverflowed())
        return raiseOverflowError(globalObject, scope, "new string is too long"_s);
    RELEASE_AND_RETURN(scope, makeStr(globalObject, builder));
}

// ---- Case ----

static ALWAYS_INLINE bool isLowercase(char32_t character) { return character < 0x80 ? isASCIILower(character) : Unicode::isLowercase(character); }
static ALWAYS_INLINE bool isUppercase(char32_t character) { return character < 0x80 ? isASCIIUpper(character) : Unicode::isUppercase(character); }
static ALWAYS_INLINE bool isTitlecase(char32_t character) { return character >= 0x80 && Unicode::isTitlecase(character); }
static ALWAYS_INLINE bool isCased(char32_t character) { return character < 0x80 ? isASCIIAlpha(character) : Unicode::isCased(character); }
static ALWAYS_INLINE bool isCaseIgnorable(char32_t character) { return Unicode::isCaseIgnorable(character); }

using Unicode::CaseMapping;

// The full mapping of one character, which may be several.
static void appendMapped(StringBuilder& builder, char32_t character, CaseMapping mapping)
{
    if (character < 0x80) {
        bool toLower = mapping == CaseMapping::Lower || mapping == CaseMapping::Fold;
        builder.append(static_cast<char>(toLower ? toASCIILower(character) : toASCIIUpper(character)));
        return;
    }
    char32_t mapped[3];
    unsigned length = Unicode::mapCase(character, mapping, mapped);
    for (unsigned i = 0; i < length; ++i)
        appendCodePoint(builder, mapped[i]);
}

// The character is the one of `text` from `start` to `end`. A capital sigma at the end of a word has a lowercase form of its own.
template<typename CharacterType>
static void appendLower(StringBuilder& builder, std::span<const CharacterType> text, size_t start, size_t end, char32_t character)
{
    if (character != 0x3a3)
        return appendMapped(builder, character, CaseMapping::Lower);
    bool isFinal = false;
    while (start) {
        char32_t before = previousCharacter(text, start);
        if (!isCaseIgnorable(before)) {
            isFinal = isCased(before);
            break;
        }
    }
    while (isFinal && end < text.size()) {
        char32_t after = nextCharacter(text, end);
        if (!isCaseIgnorable(after)) {
            isFinal = !isCased(after);
            break;
        }
    }
    builder.append(static_cast<char16_t>(isFinal ? 0x3c2 : 0x3c3));
}

// functor(text, start, end, character) for each character of the text, which is a span of its units.
template<typename Functor>
static ALWAYS_INLINE void forEachCharacter(const StrView& view, const Functor& functor)
{
    auto forEach = [&](auto text) ALWAYS_INLINE_LAMBDA {
        for (size_t end = 0; end < text.size();) {
            size_t start = end;
            char32_t character = nextCharacter(text, end);
            functor(text, start, end, character);
        }
    };
    if (view.is8Bit())
        forEach(view.string().span8());
    else
        forEach(view.string().span16());
}

static JSValue strLower(JSGlobalObject* globalObject, JSValue self)
{
    PY_STR_METHOD_PROLOGUE(view);
    if (view.string().containsOnlyASCII())
        return makeStr(vm, view.string().convertToASCIILowercase());
    StrBuilder builder;
    forEachCharacter(view, [&](auto text, size_t start, size_t end, char32_t character) {
        appendLower(builder, text, start, end, character);
    });
    RELEASE_AND_RETURN(scope, makeStr(globalObject, builder));
}

static JSValue strUpper(JSGlobalObject* globalObject, JSValue self)
{
    PY_STR_METHOD_PROLOGUE(view);
    if (view.string().containsOnlyASCII())
        return makeStr(vm, view.string().convertToASCIIUppercase());
    StrBuilder builder;
    for (char32_t character : view.view().codePoints())
        appendMapped(builder, character, CaseMapping::Upper);
    RELEASE_AND_RETURN(scope, makeStr(globalObject, builder));
}

static JSValue strCaseFold(JSGlobalObject* globalObject, JSValue self)
{
    PY_STR_METHOD_PROLOGUE(view);
    if (view.string().containsOnlyASCII())
        return makeStr(vm, view.string().convertToASCIILowercase());
    StrBuilder builder;
    for (char32_t character : view.view().codePoints())
        appendMapped(builder, character, CaseMapping::Fold);
    RELEASE_AND_RETURN(scope, makeStr(globalObject, builder));
}

static JSValue strSwapCase(JSGlobalObject* globalObject, JSValue self)
{
    PY_STR_METHOD_PROLOGUE(view);
    StrBuilder builder;
    forEachCharacter(view, [&](auto text, size_t start, size_t end, char32_t character) {
        if (isUppercase(character))
            appendLower(builder, text, start, end, character);
        else if (isLowercase(character))
            appendMapped(builder, character, CaseMapping::Upper);
        else
            appendCodePoint(builder, character);
    });
    RELEASE_AND_RETURN(scope, makeStr(globalObject, builder));
}

static JSValue strCapitalize(JSGlobalObject* globalObject, JSValue self)
{
    PY_STR_METHOD_PROLOGUE(view);
    StrBuilder builder;
    forEachCharacter(view, [&](auto text, size_t start, size_t end, char32_t character) {
        if (!start)
            appendMapped(builder, character, CaseMapping::Title);
        else
            appendLower(builder, text, start, end, character);
    });
    RELEASE_AND_RETURN(scope, makeStr(globalObject, builder));
}

static JSValue strTitle(JSGlobalObject* globalObject, JSValue self)
{
    PY_STR_METHOD_PROLOGUE(view);
    StrBuilder builder;
    bool previousIsCased = false;
    forEachCharacter(view, [&](auto text, size_t start, size_t end, char32_t character) {
        if (previousIsCased)
            appendLower(builder, text, start, end, character);
        else
            appendMapped(builder, character, CaseMapping::Title);
        previousIsCased = isCased(character);
    });
    RELEASE_AND_RETURN(scope, makeStr(globalObject, builder));
}

// ---- Predicates ----

static ALWAYS_INLINE bool isAlpha(char32_t character) { return character < 0x80 ? isASCIIAlpha(character) : Unicode::isAlpha(character); }
static ALWAYS_INLINE bool isDecimal(char32_t character) { return character < 0x80 ? isASCIIDigit(character) : Unicode::isDecimal(character); }
static ALWAYS_INLINE bool isDigit(char32_t character) { return character < 0x80 ? isASCIIDigit(character) : Unicode::isDigit(character); }
static ALWAYS_INLINE bool isNumeric(char32_t character) { return character < 0x80 ? isASCIIDigit(character) : Unicode::isNumeric(character); }
static ALWAYS_INLINE bool isAlphanumeric(char32_t character) { return isAlpha(character) || isNumeric(character); }

// True if there is at least one character and the predicate holds for all.
template<bool (*predicate)(char32_t)>
static JSValue strAll(JSGlobalObject* globalObject, JSValue self)
{
    PY_STR_METHOD_PROLOGUE(view);
    if (!view.length())
        return jsBoolean(false);
    for (char32_t character : view.view().codePoints()) {
        if (!predicate(character))
            return jsBoolean(false);
    }
    return jsBoolean(true);
}

static bool isSpaceCharacter(char32_t character) { return isPythonWhitespace(character); }
static bool isAlphaCharacter(char32_t character) { return isAlpha(character); }
static bool isDecimalCharacter(char32_t character) { return isDecimal(character); }
static bool isDigitCharacter(char32_t character) { return isDigit(character); }
static bool isNumericCharacter(char32_t character) { return isNumeric(character); }
static bool isAlphanumericCharacter(char32_t character) { return isAlphanumeric(character); }

static JSValue strIsASCII(JSGlobalObject* globalObject, JSValue self)
{
    PY_STR_METHOD_PROLOGUE(view);
    return jsBoolean(view.string().containsOnlyASCII());
}

static JSValue strIsPrintable(JSGlobalObject* globalObject, JSValue self)
{
    PY_STR_METHOD_PROLOGUE(view);
    for (char32_t character : view.view().codePoints()) {
        if (!isPrintable(character))
            return jsBoolean(false);
    }
    return jsBoolean(true);
}

static JSValue strIsIdentifier(JSGlobalObject* globalObject, JSValue self)
{
    PY_STR_METHOD_PROLOGUE(view);
    if (!view.length())
        return jsBoolean(false);
    bool first = true;
    for (char32_t character : view.view().codePoints()) {
        if (first ? !(character == '_' || isXIDStart(character)) : !isXIDContinue(character))
            return jsBoolean(false);
        first = false;
    }
    return jsBoolean(true);
}

static JSValue strIsLower(JSGlobalObject* globalObject, JSValue self)
{
    PY_STR_METHOD_PROLOGUE(view);
    bool hasCased = false;
    for (char32_t character : view.view().codePoints()) {
        if (isUppercase(character) || isTitlecase(character))
            return jsBoolean(false);
        hasCased |= isLowercase(character);
    }
    return jsBoolean(hasCased);
}

static JSValue strIsUpper(JSGlobalObject* globalObject, JSValue self)
{
    PY_STR_METHOD_PROLOGUE(view);
    bool hasCased = false;
    for (char32_t character : view.view().codePoints()) {
        if (isLowercase(character) || isTitlecase(character))
            return jsBoolean(false);
        hasCased |= isUppercase(character);
    }
    return jsBoolean(hasCased);
}

static JSValue strIsTitle(JSGlobalObject* globalObject, JSValue self)
{
    PY_STR_METHOD_PROLOGUE(view);
    bool hasCased = false;
    bool previousIsCased = false;
    for (char32_t character : view.view().codePoints()) {
        if (isUppercase(character) || isTitlecase(character)) {
            if (previousIsCased)
                return jsBoolean(false);
            previousIsCased = true;
            hasCased = true;
        } else if (isLowercase(character)) {
            if (!previousIsCased)
                return jsBoolean(false);
            previousIsCased = true;
            hasCased = true;
        } else
            previousIsCased = false;
    }
    return jsBoolean(hasCased);
}

// ---- translate ----

static JSValue strTranslate(JSGlobalObject* globalObject, JSValue self, JSValue table)
{
    PY_STR_METHOD_PROLOGUE(view);
    Runtime& rt = runtime(globalObject);
    StrBuilder builder;
    for (char32_t character : view.view().codePoints()) {
        JSValue mapped = getItem(globalObject, table, jsNumber(static_cast<uint32_t>(character)));
        if (failed(scope, mapped)) {
            // What the table does not know stays as it is.
            if (!clearPendingExceptionIfMatches(globalObject, rt.LookupError()))
                return { };
            appendCodePoint(builder, character);
            continue;
        }
        if (isNone(mapped))
            continue;
        if (isInt(mapped)) {
            auto value = tryInt64(intValue(mapped));
            if (!value || *value < 0 || *value > 0x10ffff)
                return raiseValueError(globalObject, scope, "character mapping must be in range(0x110000)"_s);
            appendCodePoint(builder, static_cast<char32_t>(*value));
            continue;
        }
        if (!isStr(mapped))
            return raiseTypeError(globalObject, scope, "character mapping must return integer, None or str"_s);
        auto text = strValue(mapped)->view(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        builder.append(text.data);
    }
    RELEASE_AND_RETURN(scope, makeStr(globalObject, builder));
}

static JSValue strMakeTrans(JSGlobalObject* globalObject, JSValue x, JSValue y, JSValue z)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyDict* result = PyDict::create(globalObject);
    if (y) {
        if (!isStr(y))
            return raiseTypeError(globalObject, scope, "maketrans() argument 2 must be str, not "_s, typeNameOfArgument(globalObject, y));
        if (z && !isStr(z))
            return raiseTypeError(globalObject, scope, "maketrans() argument 3 must be str, not "_s, typeNameOfArgument(globalObject, z));
        if (!isStr(x))
            return raiseTypeError(globalObject, scope, "first maketrans argument must be a string if there is a second argument"_s);
        StrView from(globalObject, strValue(x));
        RETURN_IF_EXCEPTION(scope, { });
        StrView to(globalObject, strValue(y));
        RETURN_IF_EXCEPTION(scope, { });
        if (from.length() != to.length())
            return raiseValueError(globalObject, scope, "the first two maketrans arguments must have equal length"_s);
        auto values = to.view().codePoints();
        auto value = values.begin();
        for (char32_t key : from.view().codePoints()) {
            result->set(globalObject, jsNumber(static_cast<uint32_t>(key)), jsNumber(static_cast<uint32_t>(*value)));
            RETURN_IF_EXCEPTION(scope, { });
            ++value;
        }
        if (z) {
            StrView removed(globalObject, strValue(z));
            RETURN_IF_EXCEPTION(scope, { });
            for (char32_t character : removed.view().codePoints()) {
                result->set(globalObject, jsNumber(static_cast<uint32_t>(character)), jsUndefined());
                RETURN_IF_EXCEPTION(scope, { });
            }
        }
        return result;
    }
    PyDict* mapping = PyDict::tryFrom(x);
    if (!mapping || !isExactDict(runtime(globalObject), mapping))
        return raiseTypeError(globalObject, scope, "if you give only one argument to maketrans it must be a dict"_s);
    JSValue key, value;
    for (unsigned position = 0; mapping->next(position, key, value);) {
        if (isStr(key)) {
            StrView view(globalObject, strValue(key));
            RETURN_IF_EXCEPTION(scope, { });
            if (view.length() != 1)
                return raiseValueError(globalObject, scope, "string keys in translatetable must be of length 1"_s);
            key = jsNumber(static_cast<uint32_t>(view.at(0)));
        } else if (!isInt(key))
            return raiseTypeError(globalObject, scope, "keys in translate table mustbe strings or integers"_s);
        result->set(globalObject, key, value);
        RETURN_IF_EXCEPTION(scope, { });
    }
    return result;
}

// ---- encode, str() ----

// False: a TypeError, "f() argument 'name' must be str, not int". Absent: null.
bool hasEmbeddedNull(JSGlobalObject* globalObject, JSString* string)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto view = string->view(globalObject);
    RETURN_IF_EXCEPTION(scope, true);
    if (view->find(static_cast<char16_t>(0)) == notFound)
        return false;
    raiseValueError(globalObject, scope, "embedded null character"_s);
    return true;
}

static bool optionalStr(JSGlobalObject* globalObject, JSValue value, ASCIILiteral function, ASCIILiteral name, JSString*& result)
{
    result = nullptr;
    if (!value)
        return true;
    if (!isStr(value)) {
        VM& vm = getVM(globalObject);
        auto scope = DECLARE_THROW_SCOPE(vm);
        raiseTypeError(globalObject, scope, function, "() argument '"_s, name, "' must be str, not "_s, isNone(value) ? "None"_str : typeName(globalObject, value));
        return false;
    }
    result = strValue(value);
    return !hasEmbeddedNull(globalObject, result);
}

static JSValue strEncodeMethod(JSGlobalObject* globalObject, JSValue self, JSValue encodingValue, JSValue errorsValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSString* encoding;
    JSString* errors;
    if (failed(scope, optionalStr(globalObject, encodingValue, "encode"_s, "encoding"_s, encoding)) || failed(scope, optionalStr(globalObject, errorsValue, "encode"_s, "errors"_s, errors)))
        return { };
    RELEASE_AND_RETURN(scope, strEncode(globalObject, strValue(self), encoding, errors));
}

static JSValue strNew(JSGlobalObject* globalObject, JSValue cls, const CallArgs& args)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyType* type = PyType::from(cls);

    if (unsigned total = args.size() + args.keywordCount(); total > 3) {
        if (!args.keywordCount() && type == rt.strType())
            return raiseTypeError(globalObject, scope, "str expected at most 3 arguments, got "_s, total);
        return raiseTypeError(globalObject, scope, "str() takes at most 3 "_s, args.size() ? ""_s : "keyword "_s, "arguments ("_s, total, " given)"_s);
    }
    JSValue values[3] = { args.get(0), args.get(1), args.get(2) };
    static constexpr ASCIILiteral names[] = { "object"_s, "encoding"_s, "errors"_s };
    for (unsigned i = 0; i < args.keywordCount(); ++i) {
        String keyword = stringOf(args.keywordName(i));
        unsigned index = 0;
        while (index < 3 && keyword != names[index])
            ++index;
        if (index == 3)
            RELEASE_AND_RETURN(scope, raiseUnexpectedKeyword(globalObject, "str"_s, keyword, names));
        if (values[index])
            return raiseTypeError(globalObject, scope, "argument for str() given by name ('"_s, keyword, "') and position ("_s, index + 1, ')');
        values[index] = args.keywordValue(i);
    }
    if (!args.keywordCount() && type == rt.strType()) {
        for (unsigned i = 1; i < 3; ++i) {
            if (values[i] && !isStr(values[i]))
                return raiseTypeError(globalObject, scope, "str() argument '"_s, names[i], "' must be str, not "_s, typeNameForMessage(globalObject, rt.typeOf(values[i])));
        }
    }
    JSString* encoding;
    JSString* errors;
    if (failed(scope, optionalStr(globalObject, values[1], "str"_s, "encoding"_s, encoding)) || failed(scope, optionalStr(globalObject, values[2], "str"_s, "errors"_s, errors)))
        return { };

    JSString* result;
    if (!values[0])
        result = jsEmptyString(vm);
    else if (!encoding && !errors) {
        JSValue object = strObject(globalObject, values[0]);
        RETURN_IF_EXCEPTION(scope, { });
        if (type == rt.strType())
            return object;
        result = strValue(object);
    } else {
        if (isStr(values[0]))
            return raiseTypeError(globalObject, scope, "decoding str is not supported"_s);
        if (!isBytesLike(values[0]))
            return raiseTypeError(globalObject, scope, "decoding to str: need a bytes-like object, "_s, typeName(globalObject, values[0]), " found"_s);
        JSValue decoded = bytesDecode(globalObject, values[0], encoding, errors);
        RETURN_IF_EXCEPTION(scope, { });
        result = strValue(decoded);
    }
    if (type == rt.strType())
        return result;
    return PyStr::create(vm, rt.structure<PyStr>(), type, result);
}

static constexpr NativeDef strMethods[] = {
    PY_NEW(strNew, "($type, *args, **kwargs)"),
    PY_SLOT("__repr__", strReprMethod, "($self, /)"),
    PY_SLOT("__str__", strStrMethod, "($self, /)"),
    PY_HASH(strHashMethod),
    PY_SLOT("__eq__", strCompareMethod<CompareOperator::Eq>, "($self, value, /)"),
    PY_SLOT("__ne__", strCompareMethod<CompareOperator::NotEq>, "($self, value, /)"),
    PY_SLOT("__lt__", strCompareMethod<CompareOperator::Lt>, "($self, value, /)"),
    PY_SLOT("__le__", strCompareMethod<CompareOperator::LtE>, "($self, value, /)"),
    PY_SLOT("__gt__", strCompareMethod<CompareOperator::Gt>, "($self, value, /)"),
    PY_SLOT("__ge__", strCompareMethod<CompareOperator::GtE>, "($self, value, /)"),
    PY_SLOT("__iter__", strIter, "($self, /)"),
    PY_SLOT("__len__", strLenMethod, "($self, /)"),
    PY_SLOT("__getitem__", strGetItem, "($self, key, /)"),
    PY_SLOT("__contains__", strContainsMethod, "($self, key, /)"),
    PY_SEQUENCE_SLOT("__add__", strAdd, "($self, value, /)"),
    PY_SEQUENCE_SLOT("__mul__", strMul, "($self, value, /)"),
    PY_SEQUENCE_SLOT("__rmul__", strMul, "($self, value, /)"),
    PY_SLOT("__mod__", strMod, "($self, value, /)"),
    PY_SLOT("__rmod__", strRMod, "($self, value, /)"),
    PY_METHOD("encode", strEncodeMethod, "($self, /, encoding='utf-8', errors='strict')"),
    PY_METHOD("replace", strReplace, "($self, old, new, /, count=-1)"),
    PY_METHOD("split", strSplit, "($self, /, sep=None, maxsplit=-1)"),
    PY_METHOD("rsplit", strRSplit, "($self, /, sep=None, maxsplit=-1)"),
    PY_METHOD("join", strJoinMethod, "($self, iterable, /)"),
    PY_METHOD("capitalize", strCapitalize, "($self, /)"),
    PY_METHOD("casefold", strCaseFold, "($self, /)"),
    PY_METHOD("title", strTitle, "($self, /)"),
    PY_METHOD("center", strJustify<Justify::Center>, "($self, width, fillchar=' ', /)"),
    PY_METHOD("count", strCount, "($self, sub[, start[, end]], /)"),
    PY_METHOD("expandtabs", strExpandTabs, "($self, /, tabsize=8)"),
    PY_METHOD("find", (strFindMethod<false, false>), "($self, sub[, start[, end]], /)"),
    PY_METHOD("partition", strPartition<false>, "($self, sep, /)"),
    PY_METHOD("index", (strFindMethod<false, true>), "($self, sub[, start[, end]], /)"),
    PY_METHOD("ljust", strJustify<Justify::Left>, "($self, width, fillchar=' ', /)"),
    PY_METHOD("lower", strLower, "($self, /)"),
    PY_METHOD("lstrip", strStrip<StripSide::Left>, "($self, chars=None, /)"),
    PY_METHOD("rfind", (strFindMethod<true, false>), "($self, sub[, start[, end]], /)"),
    PY_METHOD("rindex", (strFindMethod<true, true>), "($self, sub[, start[, end]], /)"),
    PY_METHOD("rjust", strJustify<Justify::Right>, "($self, width, fillchar=' ', /)"),
    PY_METHOD("rstrip", strStrip<StripSide::Right>, "($self, chars=None, /)"),
    PY_METHOD("rpartition", strPartition<true>, "($self, sep, /)"),
    PY_METHOD("splitlines", strSplitLines, "($self, /, keepends=False)"),
    PY_METHOD("strip", strStrip<StripSide::Both>, "($self, chars=None, /)"),
    PY_METHOD("swapcase", strSwapCase, "($self, /)"),
    PY_METHOD("translate", strTranslate, "($self, table, /)"),
    PY_METHOD("upper", strUpper, "($self, /)"),
    PY_METHOD("startswith", strStartsOrEndsWith<false>, "($self, prefix[, start[, end]], /)"),
    PY_METHOD("endswith", strStartsOrEndsWith<true>, "($self, suffix[, start[, end]], /)"),
    PY_METHOD("removeprefix", strRemovePrefix, "($self, prefix, /)"),
    PY_METHOD("removesuffix", strRemoveSuffix, "($self, suffix, /)"),
    PY_METHOD("isascii", strIsASCII, "($self, /)"),
    PY_METHOD("islower", strIsLower, "($self, /)"),
    PY_METHOD("isupper", strIsUpper, "($self, /)"),
    PY_METHOD("istitle", strIsTitle, "($self, /)"),
    PY_METHOD("isspace", strAll<isSpaceCharacter>, "($self, /)"),
    PY_METHOD("isdecimal", strAll<isDecimalCharacter>, "($self, /)"),
    PY_METHOD("isdigit", strAll<isDigitCharacter>, "($self, /)"),
    PY_METHOD("isnumeric", strAll<isNumericCharacter>, "($self, /)"),
    PY_METHOD("isalpha", strAll<isAlphaCharacter>, "($self, /)"),
    PY_METHOD("isalnum", strAll<isAlphanumericCharacter>, "($self, /)"),
    PY_METHOD("isidentifier", strIsIdentifier, "($self, /)"),
    PY_METHOD("isprintable", strIsPrintable, "($self, /)"),
    PY_METHOD("zfill", strZFill, "($self, width, /)"),
    PY_METHOD("format", strFormatMethod, "($self, /, *args, **kwargs)"),
    PY_METHOD("format_map", strFormatMap, "($self, mapping, /)"),
    PY_METHOD("__format__", strFormatSpecMethod, "($self, format_spec, /)"),
    PY_STATICMETHOD("maketrans", strMakeTrans, "(x, y=<unrepresentable>, z=<unrepresentable>, /)"),
    PY_METHOD("__sizeof__", strSizeOf, "($self, /)"),
    PY_METHOD("__getnewargs__", strGetNewArgs, "($self, /)"),
};

void initializeStrTypes(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);
    rt.strType()->initializeBuiltin(globalObject, { "str"_s, { TypeFlag::Base, TypeFlag::StrSubclass, TypeFlag::MatchSelf, TypeFlag::OwnLayout }, strMethods, { }, "str(object='') -> str\nstr(bytes_or_buffer[, encoding[, errors]]) -> str\n\nCreate a new string object from the given object. If encoding or\nerrors is specified, then the object must expose a data buffer\nthat will be decoded using the given encoding and error handler.\nOtherwise, returns the result of object.__str__() (if defined)\nor repr(object).\nencoding defaults to 'utf-8'.\nerrors defaults to 'strict'."_s }, rt.typeType(), rt.objectType());
}

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
