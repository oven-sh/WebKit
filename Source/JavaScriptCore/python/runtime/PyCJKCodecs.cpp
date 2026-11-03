/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyCJKCodecs.h"

#include "JSCInlines.h"
#include "PyCodecs.h"
#include "PyErrors.h"
#include "PyInternals.h"
#include "PyModule.h"
#include <array>
#include <atomic>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {
namespace CJK {

// The codecs for Chinese, Japanese and Korean: the modules _codecs_cn, _codecs_hk, _codecs_iso2022, _codecs_jp, _codecs_kr and
// _codecs_tw. (Modules/cjkcodecs in CPython is the reference for what they do, down to how many bytes an error covers.)
//
// A codec is a rule for how codes are written as bytes, over one or more character sets. The character sets are tables,
// PyCJKTables.inc, which tools/make_cjk_tables.py makes by asking CPython's codecs about everything: see there.

// ---- Character sets ----

struct Row {
    uint16_t offset;
    uint8_t bottom;
    uint8_t top; // Below `bottom`: the row is empty.
};

struct Override {
    uint16_t character;
    uint16_t code;
};

struct Pair {
    uint16_t first;
    uint16_t second;
    uint16_t code;
};

struct Range {
    uint16_t first;
    uint16_t last;
    uint16_t number;
};

struct TableData {
    uint8_t firstLead;
    uint8_t lastLead;
    const Row* rows;
    const uint16_t* values;
    const uint8_t* plane2;
    std::span<const Override> overrides;
    std::span<const Override> plane2Overrides;
};

#include "PyCJKTables.inc"

constexpr char32_t noCharacter = 0xFFFF;
constexpr unsigned noCode = 0xFFFF;

using Page = std::array<uint16_t, 256>;

static constexpr Page emptyPage = [] {
    Page page { };
    page.fill(noCode);
    return page;
}();

// From character to code: a page for every 256 characters of the BMP, and of plane 2.
struct CodeMap {
    std::array<const Page*, 512> pages;
};

class Table {
public:
    explicit constexpr Table(const TableData& data)
        : m_data(data)
    {
    }

    ALWAYS_INLINE char32_t character(unsigned lead, unsigned trail) const
    {
        if (lead < m_data.firstLead || lead > m_data.lastLead)
            return noCharacter;
        const Row& row = m_data.rows[lead - m_data.firstLead];
        if (trail < row.bottom || trail > row.top)
            return noCharacter;
        unsigned index = row.offset + trail - row.bottom;
        char32_t character = m_data.values[index];
        if (m_data.plane2 && (m_data.plane2[index >> 3] >> (index & 7) & 1))
            character |= 0x20000;
        return character;
    }

    ALWAYS_INLINE unsigned code(char32_t character) const
    {
        const CodeMap* map = m_codeMap.load(std::memory_order_acquire);
        if (!map) [[unlikely]]
            map = makeCodeMap();
        if (character < 0x10000)
            return (*map->pages[character >> 8])[character & 0xFF];
        if (character >> 16 == 2)
            return (*map->pages[256 + ((character >> 8) & 0xFF)])[character & 0xFF];
        return noCode;
    }

private:
    const CodeMap* makeCodeMap() const;

    const TableData& m_data;
    // Made when it is first needed, and kept for good.
    mutable std::atomic<const CodeMap*> m_codeMap { nullptr };
};

NEVER_INLINE const CodeMap* Table::makeCodeMap() const
{
    auto forEachCharacter = [&](const auto& function) {
        for (unsigned lead = m_data.firstLead; lead <= m_data.lastLead; ++lead) {
            const Row& row = m_data.rows[lead - m_data.firstLead];
            for (unsigned trail = row.bottom; trail <= row.top; ++trail) {
                if (char32_t found = character(lead, trail); found != noCharacter)
                    function((found >> 16 ? 256 : 0) + ((found >> 8) & 0xFF), found & 0xFF, lead << 8 | trail);
            }
        }
    };
    auto forEachOverride = [&](const auto& function) {
        for (auto& entry : m_data.overrides)
            function(entry.character >> 8, entry.character & 0xFF, entry.code);
        for (auto& entry : m_data.plane2Overrides)
            function(256 + (entry.character >> 8), entry.character & 0xFF, entry.code);
    };

    std::array<Page*, 512> pages { };
    unsigned count = 0;
    auto note = [&](unsigned page, unsigned, unsigned) {
        if (!pages[page]) {
            pages[page] = std::bit_cast<Page*>(static_cast<uintptr_t>(1));
            ++count;
        }
    };
    forEachCharacter(note);
    forEachOverride(note);

    auto* map = static_cast<CodeMap*>(fastMalloc(sizeof(CodeMap) + count * sizeof(Page)));
    auto* next = reinterpret_cast<Page*>(map + 1);
    for (unsigned i = 0; i < pages.size(); ++i) {
        if (pages[i]) {
            pages[i] = next++;
            *pages[i] = emptyPage;
        }
        map->pages[i] = pages[i] ? pages[i] : &emptyPage;
    }
    // Of several codes for a character the lowest is the one to write.
    forEachCharacter([&](unsigned page, unsigned index, unsigned code) {
        if ((*pages[page])[index] == noCode)
            (*pages[page])[index] = code;
    });
    forEachOverride([&](unsigned page, unsigned index, unsigned code) {
        (*pages[page])[index] = code;
    });

    const CodeMap* expected = nullptr;
    if (m_codeMap.compare_exchange_strong(expected, map, std::memory_order_acq_rel))
        return map;
    // Another thread, of another VM, was faster.
    fastFree(map);
    return expected;
}

static constinit Table jisx0208 { jisx0208Data };
static constinit Table jisx0212 { jisx0212Data };
static constinit Table jisx0213Plane1 { jisx0213Plane1Data }; // What it has over JIS X 0208.
static constinit Table jisx0213Plane2 { jisx0213Plane2Data };
static constinit Table cp932Extension { cp932ExtensionData };
static constinit Table ksx1001 { ksx1001Data };
static constinit Table cp949Extension { cp949ExtensionData };
static constinit Table gb2312 { gb2312Data };
static constinit Table gbkExtension { gbkExtensionData };
static constinit Table gb18030Extension { gb18030ExtensionData };
static constinit Table big5 { big5Data };
static constinit Table cp950Extension { cp950ExtensionData };
static constinit Table big5hkscsExtension { big5hkscsExtensionData };

// ---- Text ----

Text::Text(const String& string)
    : m_string(string)
    , m_length(string.length())
{
    if (string.is8Bit()) {
        m_narrow = string.span8().data();
        // An empty str has no characters to point to, and none are asked for.
        return;
    }
    auto units = string.span16();
    size_t pairs = 0;
    for (size_t i = 0; i + 1 < units.size(); ++i) {
        if (U16_IS_LEAD(units[i]) && U16_IS_TRAIL(units[i + 1])) {
            ++pairs;
            ++i;
        }
    }
    if (!pairs) {
        m_units = units.data();
        return;
    }
    m_length = units.size() - pairs;
    if (!m_wide.tryReserveInitialCapacity(m_length)) {
        m_hasFailed = true;
        m_length = 0;
        return;
    }
    for (size_t i = 0; i < units.size(); ++i) {
        char32_t character = units[i];
        if (U16_IS_LEAD(character) && i + 1 < units.size() && U16_IS_TRAIL(units[i + 1]))
            character = U16_GET_SUPPLEMENTARY(character, units[++i]);
        m_wide.append(character);
    }
}

// ---- Small things ----

static ALWAYS_INLINE void put(ByteBuilder& out, unsigned byte)
{
    ASSERT(byte < 0x100);
    out.append(static_cast<uint8_t>(byte));
}

template<typename... Bytes>
static ALWAYS_INLINE void put(ByteBuilder& out, unsigned first, Bytes... rest)
{
    put(out, first);
    put(out, rest...);
}

static ALWAYS_INLINE void putCode(ByteBuilder& out, unsigned code) { put(out, code >> 8, code & 0xFF); }

static ALWAYS_INLINE void put(StrBuilder& out, char32_t character)
{
    if (character < 0x10000) [[likely]]
        out.append(static_cast<char16_t>(character));
    else
        appendCodePoint(out, character);
}

// ASCII as it is, and everything else two bytes that `characterOf` knows or does not.
template<char32_t (*characterOf)(unsigned lead, unsigned trail)>
static int decodeDoubleByte(const Codec&, State&, std::span<const uint8_t> in, size_t& position, StrBuilder& out)
{
    while (position < in.size()) {
        unsigned lead = in[position];
        if (lead < 0x80) {
            put(out, lead);
            ++position;
            continue;
        }
        if (in.size() - position < 2)
            return needsMoreInput;
        char32_t character = characterOf(lead, in[position + 1]);
        if (character == noCharacter)
            return 1;
        put(out, character);
        position += 2;
    }
    return 0;
}

template<unsigned (*codeOf)(char32_t)>
static int encodeDoubleByte(const Codec&, State&, const Text& text, size_t& position, ByteBuilder& out, bool)
{
    for (; position < text.length(); ++position) {
        char32_t character = text[position];
        if (character < 0x80) {
            put(out, character);
            continue;
        }
        unsigned code = codeOf(character);
        if (code == noCode)
            return 1;
        putCode(out, code);
    }
    return 0;
}

// A code of a set of 94 by 94, with the high bits set as EUC has them.
static ALWAYS_INLINE unsigned inEUC(unsigned code) { return code == noCode ? noCode : code | 0x8080; }

// ---- Chinese, simplified: GB 2312, GBK, GB 18030, HZ ----

static char32_t gb2312Character(unsigned lead, unsigned trail) { return gb2312.character(lead ^ 0x80, trail ^ 0x80); }
static unsigned gb2312Code(char32_t character) { return inEUC(gb2312.code(character)); }

// GBK has a middle dot and a dash where GB 2312 has others that look the same.
static char32_t gbkCharacter(unsigned lead, unsigned trail)
{
    switch (lead << 8 | trail) {
    case 0xA1A4:
        return 0x00B7;
    case 0xA1AA:
        return 0x2014;
    case 0xA844:
        return 0x2015;
    }
    char32_t character = gb2312Character(lead, trail);
    return character != noCharacter ? character : gbkExtension.character(lead, trail);
}

static unsigned gbkCode(char32_t character)
{
    switch (character) {
    case 0x00B7:
        return 0xA1A4;
    case 0x2014:
        return 0xA1AA;
    case 0x2015:
        return 0xA844;
    case 0x30FB:
        return noCode;
    }
    unsigned code = gb2312Code(character);
    return code != noCode ? code : gbkExtension.code(character);
}

// The sequences of four bytes are numbered: 0x81308130 is 0.
static void putGB18030Number(ByteBuilder& out, unsigned number)
{
    put(out, 0x81 + number / 12600, 0x30 + number / 1260 % 10, 0x81 + number / 10 % 126, 0x30 + number % 10);
}

constexpr unsigned gb18030NumberOfPlane1 = (0x90 - 0x81) * 12600;

static int encodeGB18030(const Codec&, State&, const Text& text, size_t& position, ByteBuilder& out, bool)
{
    for (; position < text.length(); ++position) {
        char32_t character = text[position];
        if (character < 0x80) {
            put(out, character);
            continue;
        }
        if (character >= 0x10000) {
            putGB18030Number(out, gb18030NumberOfPlane1 + (character - 0x10000));
            continue;
        }
        unsigned code = gbkCode(character);
        if (code == noCode)
            code = gb18030Extension.code(character);
        if (code != noCode) {
            putCode(out, code);
            continue;
        }
        auto* range = std::upper_bound(std::begin(gb18030Ranges), std::end(gb18030Ranges), character, [](char32_t wanted, const Range& entry) {
            return wanted < entry.first;
        });
        if (range == std::begin(gb18030Ranges) || character > (--range)->last)
            return 1;
        putGB18030Number(out, range->number + (character - range->first));
    }
    return 0;
}

static int decodeGB18030(const Codec&, State&, std::span<const uint8_t> in, size_t& position, StrBuilder& out)
{
    while (position < in.size()) {
        unsigned lead = in[position];
        if (lead < 0x80) {
            put(out, lead);
            ++position;
            continue;
        }
        if (in.size() - position < 2)
            return needsMoreInput;
        unsigned second = in[position + 1];
        if (second >= 0x30 && second <= 0x39) {
            if (in.size() - position < 4)
                return needsMoreInput;
            unsigned third = in[position + 2];
            unsigned fourth = in[position + 3];
            if (lead < 0x81 || lead > 0xFE || third < 0x81 || third > 0xFE || fourth < 0x30 || fourth > 0x39)
                return 1;
            unsigned number = (((lead - 0x81) * 10 + second - 0x30) * 126 + third - 0x81) * 10 + fourth - 0x30;
            if (number < gb18030RangesEnd) {
                auto* range = std::upper_bound(std::begin(gb18030Ranges), std::end(gb18030Ranges), number, [](unsigned wanted, const Range& entry) {
                    return wanted < entry.number;
                }) - 1;
                put(out, range->first + (number - range->number));
            } else if (number >= gb18030NumberOfPlane1 && number - gb18030NumberOfPlane1 < 0x100000)
                put(out, 0x10000 + (number - gb18030NumberOfPlane1));
            else
                return 1;
            position += 4;
            continue;
        }
        char32_t character = gbkCharacter(lead, second);
        if (character == noCharacter)
            character = gb18030Extension.character(lead, second);
        if (character == noCharacter)
            return 1;
        put(out, character);
        position += 2;
    }
    return 0;
}

// HZ: GB 2312 in seven bits between ~{ and ~}. c[0]: whether it is between them.

static void startHZ(State& state) { state.c[0] = 0; }

static void finishEncodingHZ(State& state, ByteBuilder& out)
{
    if (state.c[0])
        put(out, '~', '}');
    state.c[0] = 0;
}

static int encodeHZ(const Codec&, State& state, const Text& text, size_t& position, ByteBuilder& out, bool)
{
    for (; position < text.length(); ++position) {
        char32_t character = text[position];
        if (character < 0x80) {
            finishEncodingHZ(state, out);
            put(out, character);
            if (character == '~')
                put(out, '~');
            continue;
        }
        unsigned code = gb2312.code(character);
        if (code == noCode)
            return 1;
        if (!state.c[0]) {
            put(out, '~', '{');
            state.c[0] = 1;
        }
        putCode(out, code);
    }
    return 0;
}

static int decodeHZ(const Codec&, State& state, std::span<const uint8_t> in, size_t& position, StrBuilder& out)
{
    while (position < in.size()) {
        unsigned byte = in[position];
        if (byte == '~') {
            if (in.size() - position < 2)
                return needsMoreInput;
            unsigned second = in[position + 1];
            if (state.c[0]) {
                if (second != '}')
                    return 1;
                state.c[0] = 0;
            } else if (second == '~')
                put(out, '~');
            else if (second == '{')
                state.c[0] = 1;
            else if (second != '\n') // A line that goes on.
                return 1;
            position += 2;
            continue;
        }
        if (byte & 0x80)
            return 1;
        if (!state.c[0]) {
            put(out, byte);
            ++position;
            continue;
        }
        if (in.size() - position < 2)
            return needsMoreInput;
        char32_t character = gb2312.character(byte, in[position + 1]);
        if (character == noCharacter)
            return 1;
        put(out, character);
        position += 2;
    }
    return 0;
}

// ---- Chinese, traditional: Big5, code page 950, Big5-HKSCS ----

static char32_t big5Character(unsigned lead, unsigned trail) { return big5.character(lead, trail); }
static unsigned big5Code(char32_t character) { return big5.code(character); }

static char32_t cp950Character(unsigned lead, unsigned trail)
{
    char32_t character = cp950Extension.character(lead, trail);
    return character != noCharacter ? character : big5.character(lead, trail);
}

static unsigned cp950Code(char32_t character)
{
    unsigned code = cp950Extension.code(character);
    return code != noCode ? code : big5.code(character);
}

// Four codes of HKSCS stand for an E with a circumflex and, as a second character, a macron or a caron.
static int encodeBig5HKSCS(const Codec&, State&, const Text& text, size_t& position, ByteBuilder& out, bool flush)
{
    for (; position < text.length(); ++position) {
        char32_t character = text[position];
        if (character < 0x80) {
            put(out, character);
            continue;
        }
        if (character == 0xCA || character == 0xEA) {
            if (position + 1 == text.length() && !flush)
                return needsMoreInput;
            char32_t mark = position + 1 < text.length() ? text[position + 1] : 0;
            unsigned base = character == 0xCA ? 0x8862 : 0x88A3;
            if (mark == 0x304 || mark == 0x30C) {
                putCode(out, base + (mark == 0x304 ? 0 : 2));
                ++position;
            } else
                putCode(out, base + 4);
            continue;
        }
        unsigned code = big5hkscsExtension.code(character);
        if (code == noCode)
            code = big5.code(character);
        if (code == noCode)
            return 1;
        putCode(out, code);
    }
    return 0;
}

static int decodeBig5HKSCS(const Codec&, State&, std::span<const uint8_t> in, size_t& position, StrBuilder& out)
{
    while (position < in.size()) {
        unsigned lead = in[position];
        if (lead < 0x80) {
            put(out, lead);
            ++position;
            continue;
        }
        if (in.size() - position < 2)
            return needsMoreInput;
        unsigned trail = in[position + 1];
        unsigned code = lead << 8 | trail;
        // What Big5 has from 0xC6A1 to 0xC8FE, HKSCS has otherwise.
        char32_t character = code < 0xC6A1 || code > 0xC8FF ? big5.character(lead, trail) : noCharacter;
        if (character == noCharacter)
            character = big5hkscsExtension.character(lead, trail);
        if (character != noCharacter)
            put(out, character);
        else if (code == 0x8862 || code == 0x8864 || code == 0x88A3 || code == 0x88A5) {
            put(out, code < 0x88A3 ? 0xCA : 0xEA);
            put(out, code == 0x8862 || code == 0x88A3 ? 0x304 : 0x30C);
        } else
            return 1;
        position += 2;
    }
    return 0;
}

// ---- Korean: EUC-KR, code page 949, Johab ----

// A syllable is made of a first consonant, a vowel and perhaps a last consonant. Which of the letters from U+3131 on, in the order of
// row 4 of KS X 1001, each of the consonants is; the 21 vowels follow one another from the 31st.
constexpr uint8_t letterOfFirstConsonant[19] = { 0, 1, 3, 6, 7, 8, 16, 17, 18, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29 };
constexpr uint8_t letterOfLastConsonant[28] = { 0xFF, 0, 1, 2, 3, 4, 5, 6, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 19, 20, 21, 22, 23, 25, 26, 27, 28, 29 };
constexpr unsigned firstVowelLetter = 30;
constexpr unsigned letterCount = 51;
constexpr unsigned notThere = 0xFF;

template<size_t size>
static constexpr unsigned indexOfLetter(const uint8_t (&letters)[size], unsigned letter)
{
    for (unsigned i = 0; i < size; ++i) {
        if (letters[i] == letter)
            return i;
    }
    return notThere;
}

constexpr char32_t firstSyllable = 0xAC00;
constexpr char32_t lastSyllable = 0xD7A3;
static constexpr char32_t syllable(unsigned first, unsigned vowel, unsigned last) { return firstSyllable + (first * 21 + vowel) * 28 + last; }

// The syllables KS X 1001 has no code for are spelt in EUC-KR: a filler, and three letters of row 4.
constexpr unsigned rowOfLetters = 0xA4;
constexpr unsigned filler = 0xD4;

static int encodeEUCKR(const Codec&, State&, const Text& text, size_t& position, ByteBuilder& out, bool)
{
    for (; position < text.length(); ++position) {
        char32_t character = text[position];
        if (character < 0x80) {
            put(out, character);
            continue;
        }
        if (unsigned code = ksx1001.code(character); code != noCode) {
            putCode(out, code | 0x8080);
            continue;
        }
        if (cp949Extension.code(character) == noCode)
            return 1;
        ASSERT(character >= firstSyllable && character <= lastSyllable);
        unsigned index = character - firstSyllable;
        unsigned last = index % 28;
        put(out, rowOfLetters, filler, rowOfLetters, 0xA1 + letterOfFirstConsonant[index / 588]);
        put(out, rowOfLetters, 0xA1 + firstVowelLetter + index / 28 % 21, rowOfLetters, last ? 0xA1 + letterOfLastConsonant[last] : filler);
    }
    return 0;
}

static int decodeEUCKR(const Codec&, State&, std::span<const uint8_t> in, size_t& position, StrBuilder& out)
{
    while (position < in.size()) {
        unsigned lead = in[position];
        if (lead < 0x80) {
            put(out, lead);
            ++position;
            continue;
        }
        if (in.size() - position < 2)
            return needsMoreInput;
        unsigned trail = in[position + 1];
        if (lead == rowOfLetters && trail == filler) {
            if (in.size() - position < 8)
                return needsMoreInput;
            auto* bytes = in.data() + position;
            if (bytes[2] != rowOfLetters || bytes[4] != rowOfLetters || bytes[6] != rowOfLetters)
                return 1;
            unsigned first = indexOfLetter(letterOfFirstConsonant, bytes[3] - 0xA1);
            unsigned vowel = bytes[5] - 0xA1 - firstVowelLetter;
            unsigned last = bytes[7] == filler ? 0 : indexOfLetter(letterOfLastConsonant, bytes[7] - 0xA1);
            if (bytes[3] < 0xA1 || bytes[7] < 0xA1 || first == notThere || vowel >= 21 || last == notThere)
                return 1;
            put(out, syllable(first, vowel, last));
            position += 8;
            continue;
        }
        char32_t character = ksx1001.character(lead ^ 0x80, trail ^ 0x80);
        if (character == noCharacter)
            return 1;
        put(out, character);
        position += 2;
    }
    return 0;
}

static char32_t cp949Character(unsigned lead, unsigned trail)
{
    char32_t character = ksx1001.character(lead ^ 0x80, trail ^ 0x80);
    return character != noCharacter ? character : cp949Extension.character(lead, trail);
}

static unsigned cp949Code(char32_t character)
{
    unsigned code = inEUC(ksx1001.code(character));
    return code != noCode ? code : cp949Extension.code(character);
}

// Johab: a syllable is a set bit and five bits for each of its three parts. 1, 2 and 1 say that the part is missing, which leaves
// a letter by itself. The rest of KS X 1001 comes after, two rows to a lead byte.
constexpr unsigned johabNoFirst = 1;
constexpr unsigned johabNoVowel = 2;
constexpr unsigned johabNoLast = 1;
static constexpr unsigned johabFirst(unsigned index) { return index + 2; }
static constexpr unsigned johabVowel(unsigned index) { return index + 3 + (index + 1) / 6 * 2; }
static constexpr unsigned johabLast(unsigned index) { return index + 1 + (index >= 17); }
static constexpr unsigned johabCode(unsigned first, unsigned vowel, unsigned last) { return 0x8000 | first << 10 | vowel << 5 | last; }

// The index of what the five bits stand for; notThere if for nothing. `count` indices there are.
template<unsigned (*bitsOf)(unsigned), unsigned count>
static unsigned johabIndex(unsigned bits)
{
    for (unsigned i = 0; i < count; ++i) {
        if (bitsOf(i) == bits)
            return i;
    }
    return notThere;
}

static unsigned johabCodeOf(char32_t character)
{
    if (character >= firstSyllable && character <= lastSyllable) {
        unsigned index = character - firstSyllable;
        return johabCode(johabFirst(index / 588), johabVowel(index / 28 % 21), johabLast(index % 28));
    }
    if (character >= 0x3131 && character < 0x3131 + letterCount) {
        unsigned letter = character - 0x3131;
        if (letter >= firstVowelLetter)
            return johabCode(johabNoFirst, johabVowel(letter - firstVowelLetter), johabNoLast);
        if (unsigned first = indexOfLetter(letterOfFirstConsonant, letter); first != notThere)
            return johabCode(johabFirst(first), johabNoVowel, johabNoLast);
        return johabCode(johabNoFirst, johabNoVowel, johabLast(indexOfLetter(letterOfLastConsonant, letter)));
    }
    unsigned code = ksx1001.code(character);
    if (code == noCode)
        return noCode;
    unsigned row = (code >> 8) - 0x21;
    unsigned cell = (code & 0xFF) - 0x21;
    // Symbols, and Chinese characters. Between them are the syllables.
    if (row > 0x2C - 0x21 && (row < 0x4A - 0x21 || row > 0x7D - 0x21))
        return noCode;
    row += row < 0x4A - 0x21 ? 0x1B2 : 0x197;
    if (row & 1)
        cell += 94;
    return (row >> 1) << 8 | (cell + (cell < 0x4E ? 0x31 : 0x43));
}

static char32_t johabCharacter(unsigned lead, unsigned trail)
{
    if (lead < 0xD8) {
        unsigned firstBits = (lead >> 2) & 0x1F;
        unsigned vowelBits = (lead << 3 | trail >> 5) & 0x1F;
        unsigned lastBits = trail & 0x1F;
        unsigned first = johabIndex<johabFirst, 19>(firstBits);
        unsigned vowel = johabIndex<johabVowel, 21>(vowelBits);
        unsigned last = johabIndex<johabLast, 28>(lastBits); // Nothing last is index 0.
        bool hasFirst = firstBits != johabNoFirst;
        bool hasVowel = vowelBits != johabNoVowel;
        bool hasLast = lastBits != johabNoLast;
        if ((hasFirst && first == notThere) || (hasVowel && vowel == notThere) || last == notThere)
            return noCharacter;
        if (hasFirst && hasVowel)
            return syllable(first, vowel, last);
        if (hasFirst + hasVowel + hasLast > 1)
            return noCharacter;
        if (hasFirst)
            return 0x3131 + letterOfFirstConsonant[first];
        if (hasVowel)
            return 0x3131 + firstVowelLetter + vowel;
        if (hasLast)
            return 0x3131 + letterOfLastConsonant[last];
        return 0x3000;
    }
    if (lead == 0xDF || lead > 0xF9 || trail < 0x31 || (trail >= 0x80 && trail < 0x91) || (trail & 0x7F) == 0x7F)
        return noCharacter;
    // The letters have codes as parts of syllables.
    if (lead == 0xDA && trail >= 0xA1 && trail <= 0xD3)
        return noCharacter;
    unsigned row = lead < 0xE0 ? 2 * (lead - 0xD9) : 2 * lead - 0x197;
    unsigned cell = trail - (trail < 0x91 ? 0x31 : 0x43);
    // (0xD8 would be the two rows before the first.)
    return ksx1001.character((row + cell / 94 + 0x21) & 0xFF, cell % 94 + 0x21);
}

// ---- Japanese: Shift_JIS, code page 932, EUC-JP, and the two for JIS X 0213 ----

constexpr char32_t yenSign = 0xA5;
constexpr char32_t overline = 0x203E;
constexpr char32_t fullwidthBackslash = 0xFF3C;
constexpr char32_t fullwidthTilde = 0xFF5E;
constexpr unsigned codeOfBackslash = 0x2140;
constexpr unsigned codeOfTilde = 0x2232;

// The katakana of half width, U+FF61 to U+FF9F, are the bytes 0xA1 to 0xDF of JIS X 0201.
static ALWAYS_INLINE bool isHalfwidthKatakana(char32_t character) { return character >= 0xFF61 && character <= 0xFF9F; }
static ALWAYS_INLINE bool isKatakanaByte(unsigned byte) { return byte >= 0xA1 && byte <= 0xDF; }
constexpr unsigned katakanaOffset = 0xFEC0;

// Shift_JIS packs two rows of 94 into a lead byte, around the bytes of JIS X 0201. `row` and `cell` count from 0; the rows of a
// second plane follow those of the first.
static ALWAYS_INLINE void putShiftJIS(ByteBuilder& out, unsigned row, unsigned cell)
{
    unsigned lead = row >> 1;
    unsigned trail = cell + (row & 1 ? 94 : 0);
    put(out, lead + (lead < 0x1F ? 0x81 : 0xC1), trail + (trail < 0x3F ? 0x40 : 0x41));
}

static ALWAYS_INLINE void putShiftJISCode(ByteBuilder& out, unsigned code) { putShiftJIS(out, (code >> 8) - 0x21, (code & 0xFF) - 0x21); }

static ALWAYS_INLINE bool isShiftJISTrail(unsigned trail) { return trail >= 0x40 && trail <= 0xFC && trail != 0x7F; }

static ALWAYS_INLINE void fromShiftJIS(unsigned lead, unsigned trail, unsigned& row, unsigned& cell)
{
    row = (lead - (lead < 0xE0 ? 0x81 : 0xC1)) * 2;
    cell = trail - (trail < 0x80 ? 0x40 : 0x41);
    if (cell >= 94) {
        ++row;
        cell -= 94;
    }
}

static int encodeShiftJIS(const Codec&, State&, const Text& text, size_t& position, ByteBuilder& out, bool)
{
    for (; position < text.length(); ++position) {
        char32_t character = text[position];
        if (character < 0x80)
            put(out, character);
        else if (character == yenSign)
            put(out, 0x5C);
        else if (character == overline)
            put(out, 0x7E);
        else if (isHalfwidthKatakana(character))
            put(out, character - katakanaOffset);
        else {
            unsigned code = character == fullwidthBackslash ? codeOfBackslash : jisx0208.code(character);
            if (code == noCode)
                return 1;
            putShiftJISCode(out, code);
        }
    }
    return 0;
}

static int decodeShiftJIS(const Codec&, State&, std::span<const uint8_t> in, size_t& position, StrBuilder& out)
{
    while (position < in.size()) {
        unsigned lead = in[position];
        if (lead < 0x80 || isKatakanaByte(lead)) {
            put(out, lead < 0x80 ? lead : lead + katakanaOffset);
            ++position;
            continue;
        }
        if (lead == 0x80 || lead == 0xA0 || lead > 0xEA)
            return 1;
        if (in.size() - position < 2)
            return needsMoreInput;
        unsigned trail = in[position + 1];
        if (!isShiftJISTrail(trail))
            return 1;
        unsigned row, cell;
        fromShiftJIS(lead, trail, row, cell);
        char32_t character = !row && cell == (codeOfBackslash & 0xFF) - 0x21 ? fullwidthBackslash : jisx0208.character(row + 0x21, cell + 0x21);
        if (character == noCharacter)
            return 1;
        put(out, character);
        position += 2;
    }
    return 0;
}

// Microsoft's: more characters, some others, 1880 for the user from U+E000, and a character for every single byte.
constexpr char32_t cp932FirstOfUser = 0xE000;
constexpr unsigned cp932CountOfUser = 10 * 188;

static int encodeCP932(const Codec&, State&, const Text& text, size_t& position, ByteBuilder& out, bool)
{
    for (; position < text.length(); ++position) {
        char32_t character = text[position];
        if (character <= 0x80)
            put(out, character);
        else if (isHalfwidthKatakana(character))
            put(out, character - katakanaOffset);
        else if (character == 0xF8F0)
            put(out, 0xA0);
        else if (character >= 0xF8F1 && character <= 0xF8F3)
            put(out, character - 0xF8F1 + 0xFD);
        else if (unsigned code = cp932Extension.code(character); code != noCode)
            putCode(out, code);
        else if (code = jisx0208.code(character); code != noCode)
            putShiftJISCode(out, code);
        else if (character >= cp932FirstOfUser && character < cp932FirstOfUser + cp932CountOfUser)
            putShiftJIS(out, (0xF0 - 0xC1) * 2 + (character - cp932FirstOfUser) / 94, (character - cp932FirstOfUser) % 94);
        else
            return 1;
    }
    return 0;
}

static int decodeCP932(const Codec&, State&, std::span<const uint8_t> in, size_t& position, StrBuilder& out)
{
    while (position < in.size()) {
        unsigned lead = in[position];
        if (lead <= 0x80 || (lead >= 0xA0 && lead <= 0xDF) || lead >= 0xFD) {
            put(out, lead <= 0x80 ? lead : lead == 0xA0 ? 0xF8F0 : lead >= 0xFD ? 0xF8F1 + (lead - 0xFD) : lead + katakanaOffset);
            ++position;
            continue;
        }
        if (in.size() - position < 2)
            return needsMoreInput;
        unsigned trail = in[position + 1];
        char32_t character = cp932Extension.character(lead, trail);
        if (character == noCharacter) {
            if (!isShiftJISTrail(trail) || (lead > 0xEA && lead < 0xF0) || lead > 0xF9)
                return 1;
            unsigned row, cell;
            fromShiftJIS(lead, trail, row, cell);
            character = lead >= 0xF0 ? cp932FirstOfUser + (row - (0xF0 - 0xC1) * 2) * 94 + cell : jisx0208.character(row + 0x21, cell + 0x21);
            if (character == noCharacter)
                return 1;
        }
        put(out, character);
        position += 2;
    }
    return 0;
}

// EUC-JP: JIS X 0208 with the high bits set, katakana of half width after 0x8E, JIS X 0212 after 0x8F.

static int encodeEUCJP(const Codec&, State&, const Text& text, size_t& position, ByteBuilder& out, bool)
{
    for (; position < text.length(); ++position) {
        char32_t character = text[position];
        if (character < 0x80)
            put(out, character);
        else if (unsigned code = jisx0208.code(character); code != noCode)
            putCode(out, code | 0x8080);
        else if (code = jisx0212.code(character); code != noCode)
            put(out, 0x8F, code >> 8 | 0x80, (code & 0xFF) | 0x80);
        else if (isHalfwidthKatakana(character))
            put(out, 0x8E, character - katakanaOffset);
        else if (character == fullwidthBackslash)
            putCode(out, codeOfBackslash | 0x8080);
        else if (character == yenSign)
            put(out, 0x5C);
        else if (character == overline)
            put(out, 0x7E);
        else
            return 1;
    }
    return 0;
}

static int decodeEUCJP(const Codec&, State&, std::span<const uint8_t> in, size_t& position, StrBuilder& out)
{
    while (position < in.size()) {
        unsigned lead = in[position];
        if (lead < 0x80) {
            put(out, lead);
            ++position;
            continue;
        }
        unsigned length = lead == 0x8F ? 3 : 2;
        if (in.size() - position < length)
            return needsMoreInput;
        unsigned second = in[position + 1];
        char32_t character;
        if (lead == 0x8E)
            character = isKatakanaByte(second) ? second + katakanaOffset : noCharacter;
        else if (lead == 0x8F)
            character = jisx0212.character(second ^ 0x80, in[position + 2] ^ 0x80);
        else if ((lead << 8 | second) == (codeOfBackslash | 0x8080))
            character = fullwidthBackslash;
        else
            character = jisx0208.character(lead ^ 0x80, second ^ 0x80);
        if (character == noCharacter)
            return 1;
        put(out, character);
        position += length;
    }
    return 0;
}

// JIS X 0213 is JIS X 0208 with more in it, and a second plane. There are two editions: that of 2004 added ten characters, and
// changed its mind about one. The codecs named after the edition of 2000 are those of 2004 without that. The configuration of
// a codec says which it is.
static constexpr int edition2000 = 2000;
static ALWAYS_INLINE bool isEdition2000(const Codec& codec) { return codec.configuration == &edition2000; }

static bool wasAddedIn2004(char32_t character)
{
    switch (character) {
    case 0x4FF1:
    case 0x525D:
    case 0x541E:
    case 0x5653:
    case 0x59F8:
    case 0x5C5B:
    case 0x5E77:
    case 0x7626:
    case 0x7E6B:
    case 0x9B1C:
    case 0x20B9F:
        return true;
    }
    return false;
}

static bool wasAddedToPlane1In2004(unsigned code)
{
    switch (code) {
    case 0x2E21:
    case 0x2F7E:
    case 0x4F54:
    case 0x4F7E:
    case 0x7427:
        return true;
    }
    return code >= 0x7E7A && code <= 0x7E7E;
}

constexpr unsigned codeChangedIn2004 = 0x7D3B; // In plane 2.
constexpr char32_t whatItWasIn2000 = 0x9B1D;

constexpr unsigned inPlane2 = 0x8000;
constexpr unsigned dependsOnNext = 0xFFFE;

// The code of a character in JIS X 0213, with `inPlane2` if it is there. Not for what JIS X 0208 has, but for the characters that,
// with another after them, have a code together: for those, `dependsOnNext`.
static unsigned jisx0213Code(char32_t character, bool is2000)
{
    if (is2000) {
        if (wasAddedIn2004(character))
            return noCode;
        if (character == whatItWasIn2000)
            return codeChangedIn2004 | inPlane2;
    }
    if (character <= jisx0213PairStarters[std::size(jisx0213PairStarters) - 1].character) {
        for (auto& starter : jisx0213PairStarters) {
            if (starter.character == character)
                return dependsOnNext;
        }
    }
    unsigned code = jisx0213Plane1.code(character);
    if (code != noCode)
        return code;
    code = jisx0213Plane2.code(character);
    return code != noCode ? code | inPlane2 : noCode;
}

static unsigned jisx0213CodeOfPair(char32_t first, char32_t second)
{
    for (auto& pair : jisx0213Pairs) {
        if (pair.first == first && pair.second == second)
            return pair.code;
    }
    return noCode;
}

static unsigned jisx0213CodeAlone(char32_t character)
{
    for (auto& starter : jisx0213PairStarters) {
        if (starter.character == character)
            return starter.code;
    }
    return noCode;
}

// For the two codecs below: the code of the character at the position, whichever of the sets has it. `consumed` is 2 if it is the
// code of that character and the next. With `inPlane2`, it is of plane 2, or of JIS X 0212: `isOf0212` tells. Or needsMoreInput,
// as a negative number is none of the codes.
static int jisx0213CodeAt(const Codec& codec, const Text& text, size_t position, bool flush, size_t& consumed, bool& isOf0212)
{
    char32_t character = text[position];
    bool is2000 = isEdition2000(codec);
    consumed = 1;
    isOf0212 = false;
    if (is2000 && wasAddedIn2004(character))
        return noCode;
    unsigned code = jisx0213Code(character, is2000);
    if (code == dependsOnNext) {
        if (position + 1 == text.length()) {
            if (!flush)
                return needsMoreInput;
            return jisx0213CodeAlone(character);
        }
        code = jisx0213CodeOfPair(character, text[position + 1]);
        if (code == noCode)
            return jisx0213CodeAlone(character);
        consumed = 2;
        return code;
    }
    if (code != noCode)
        return code;
    code = jisx0208.code(character);
    if (code != noCode)
        return code;
    code = jisx0212.code(character);
    if (code == noCode)
        return noCode;
    isOf0212 = true;
    return code | inPlane2;
}

// What a code of plane 1 stands for. Two characters: the first, and `second` is set.
static char32_t jisx0213Plane1Character(unsigned lead, unsigned trail, char32_t& second)
{
    char32_t character = jisx0208.character(lead, trail);
    if (character != noCharacter)
        return character;
    character = jisx0213Plane1.character(lead, trail);
    if (character != noCharacter)
        return character;
    for (auto& pair : jisx0213Pairs) {
        if (pair.code == (lead << 8 | trail)) {
            second = pair.second;
            return pair.first;
        }
    }
    return noCharacter;
}

static int encodeEUCJIS2004(const Codec& codec, State&, const Text& text, size_t& position, ByteBuilder& out, bool flush)
{
    while (position < text.length()) {
        char32_t character = text[position];
        if (character < 0x80) {
            put(out, character);
            ++position;
            continue;
        }
        size_t consumed;
        bool isOf0212;
        int code = jisx0213CodeAt(codec, text, position, flush, consumed, isOf0212);
        if (code < 0)
            return code;
        if (code == noCode && !(isEdition2000(codec) && wasAddedIn2004(character))) {
            if (isHalfwidthKatakana(character)) {
                put(out, 0x8E, character - katakanaOffset);
                ++position;
                continue;
            }
            if (character == fullwidthBackslash)
                code = codeOfBackslash;
            else if (character == fullwidthTilde)
                code = codeOfTilde;
        }
        if (code == noCode)
            return 1;
        if (code & inPlane2)
            put(out, 0x8F);
        putCode(out, code | 0x8080);
        position += consumed;
    }
    return 0;
}

static int decodeEUCJIS2004(const Codec& codec, State&, std::span<const uint8_t> in, size_t& position, StrBuilder& out)
{
    bool is2000 = isEdition2000(codec);
    while (position < in.size()) {
        unsigned lead = in[position];
        if (lead < 0x80) {
            put(out, lead);
            ++position;
            continue;
        }
        unsigned length = lead == 0x8F ? 3 : 2;
        if (in.size() - position < length)
            return needsMoreInput;
        unsigned first = in[position + length - 2] ^ 0x80;
        unsigned trail = in[position + length - 1] ^ 0x80;
        unsigned code = first << 8 | trail;
        char32_t character;
        char32_t second = 0;
        if (lead == 0x8E)
            character = isKatakanaByte(trail ^ 0x80) ? (trail ^ 0x80) + katakanaOffset : noCharacter;
        else if (lead == 0x8F) {
            if (is2000 && code == codeChangedIn2004)
                character = whatItWasIn2000;
            else {
                character = jisx0213Plane2.character(first, trail);
                if (character == noCharacter)
                    character = jisx0212.character(first, trail);
            }
        } else if (is2000 && wasAddedToPlane1In2004(code))
            return 2;
        else if (code == codeOfBackslash)
            character = fullwidthBackslash;
        else if (code == codeOfTilde)
            character = fullwidthTilde;
        else
            character = jisx0213Plane1Character(first, trail, second);
        if (character == noCharacter)
            return 1;
        put(out, character);
        if (second)
            put(out, second);
        position += length;
    }
    return 0;
}

// In Shift_JIS the 26 rows of plane 2 that are in use come after the 94 of plane 1, not in their order: the first nine fill the gaps
// that the rows of JIS X 0212 in use leave.
constexpr uint8_t firstRowsOfPlane2[] = { 1, 8, 3, 4, 5, 12, 13, 14, 15 };
constexpr unsigned firstOfLastRowsOfPlane2 = 78;

static int encodeShiftJIS2004(const Codec& codec, State&, const Text& text, size_t& position, ByteBuilder& out, bool flush)
{
    while (position < text.length()) {
        char32_t character = text[position];
        // The single bytes are JIS X 0201, which has a yen sign and an overline where ASCII has a backslash and a tilde.
        if ((character < 0x80 && character != 0x5C && character != 0x7E) || character == yenSign || character == overline || isHalfwidthKatakana(character)) {
            put(out, character < 0x80 ? character : character == yenSign ? 0x5C : character == overline ? 0x7E : character - katakanaOffset);
            ++position;
            continue;
        }
        size_t consumed;
        bool isOf0212;
        int code = jisx0213CodeAt(codec, text, position, flush, consumed, isOf0212);
        if (code < 0)
            return code;
        if (code == noCode || isOf0212)
            return 1;
        unsigned row = ((code & ~inPlane2) >> 8) - 0x21;
        if (code & inPlane2) {
            if (row + 1 >= firstOfLastRowsOfPlane2)
                row = 94 + std::size(firstRowsOfPlane2) + (row + 1 - firstOfLastRowsOfPlane2);
            else {
                unsigned index = 0;
                while (firstRowsOfPlane2[index] != row + 1)
                    ++index;
                row = 94 + index;
            }
        }
        putShiftJIS(out, row, (code & 0xFF) - 0x21);
        position += consumed;
    }
    return 0;
}

static int decodeShiftJIS2004(const Codec& codec, State&, std::span<const uint8_t> in, size_t& position, StrBuilder& out)
{
    bool is2000 = isEdition2000(codec);
    while (position < in.size()) {
        unsigned lead = in[position];
        if (lead < 0x80 || isKatakanaByte(lead)) {
            put(out, lead == 0x5C ? yenSign : lead == 0x7E ? overline : lead < 0x80 ? lead : lead + katakanaOffset);
            ++position;
            continue;
        }
        if (lead == 0x80 || lead == 0xA0 || lead > 0xFC)
            return 1;
        if (in.size() - position < 2)
            return needsMoreInput;
        unsigned trail = in[position + 1];
        if (!isShiftJISTrail(trail))
            return 1;
        unsigned row, cell;
        fromShiftJIS(lead, trail, row, cell);
        char32_t character;
        char32_t second = 0;
        if (row < 94) {
            if (is2000 && wasAddedToPlane1In2004((row + 0x21) << 8 | (cell + 0x21)))
                return 2;
            character = jisx0213Plane1Character(row + 0x21, cell + 0x21, second);
        } else {
            row -= 94;
            row = row < std::size(firstRowsOfPlane2) ? firstRowsOfPlane2[row] : row - std::size(firstRowsOfPlane2) + firstOfLastRowsOfPlane2;
            if (is2000 && ((row + 0x20) << 8 | (cell + 0x21)) == codeChangedIn2004)
                character = whatItWasIn2000;
            else
                character = jisx0213Plane2.character(row + 0x20, cell + 0x21);
        }
        if (character == noCharacter)
            return 1;
        put(out, character);
        if (second)
            put(out, second);
        position += 2;
    }
    return 0;
}

// ---- ISO 2022: seven bits, and escape sequences that say which character set they stand for ----
//
// c[0] to c[3]: the sets designated as G0 to G3, each by the last byte of the sequence that designates it, with the high bit set
// for a set of two bytes. c[4]: flags.

constexpr uint8_t escape = 0x1B;
constexpr uint8_t shiftOut = 0x0E;
constexpr uint8_t shiftIn = 0x0F;

constexpr uint8_t twoBytes = 0x80;
constexpr uint8_t setASCII = 'B';
constexpr uint8_t setLatin1 = 'A';
constexpr uint8_t setGreek = 'F';
constexpr uint8_t setKatakana = 'I';
constexpr uint8_t setRoman = 'J';
constexpr uint8_t setGB2312 = 'A' | twoBytes;
constexpr uint8_t setJISX0208 = 'B' | twoBytes;
constexpr uint8_t setKSX1001 = 'C' | twoBytes;
constexpr uint8_t setJISX0212 = 'D' | twoBytes;
constexpr uint8_t setJISX0213Plane1Of2000 = 'O' | twoBytes;
constexpr uint8_t setJISX0213Plane2 = 'P' | twoBytes;
constexpr uint8_t setJISX0213Plane1Of2004 = 'Q' | twoBytes;
constexpr uint8_t setJISX0208Of1978 = '@' | twoBytes;

constexpr uint8_t isShifted = 1; // G1 is in use, not G0.
constexpr uint8_t isInUnknownSequence = 2; // An escape sequence that is none of ISO 2022's is passed on.

// What a set makes of one or two characters. `length`: 1; or, after the answer dependsOnNext, 2, or -1 if there is no second
// character. It is then set to how many characters the code is for.
using ISO2022Encoder = unsigned (*)(const char32_t* characters, int& length);
// Two characters come as first << 16 | second.
using ISO2022Decoder = char32_t (*)(const uint8_t*);

struct Designation {
    uint8_t set;
    uint8_t plane;
    uint8_t width;
    ISO2022Decoder decode;
    ISO2022Encoder encode;
};

struct ISO2022 {
    bool hasShifts; // SO and SI choose between G0 and G1.
    bool hasG2;
    bool hasRevisions; // ESC & @ before ESC $ B announces the edition of 1990 of JIS X 0208.
    std::span<const Designation> designations;
};

static ALWAYS_INLINE const ISO2022& iso2022Of(const Codec& codec) { return *static_cast<const ISO2022*>(codec.configuration); }

static void startEncodingISO2022(State& state)
{
    state.c[0] = setASCII;
    state.c[1] = setASCII;
    state.c[4] = 0;
}

static void finishEncodingISO2022(State& state, ByteBuilder& out)
{
    if (state.c[4] & isShifted) {
        put(out, shiftIn);
        state.c[4] &= ~isShifted;
    }
    if (state.c[0] != setASCII) {
        put(out, escape, '(', 'B');
        state.c[0] = setASCII;
    }
}

static int encodeISO2022(const Codec& codec, State& state, const Text& text, size_t& position, ByteBuilder& out, bool flush)
{
    const ISO2022& configuration = iso2022Of(codec);
    while (position < text.length()) {
        char32_t character = text[position];
        if (character < 0x80) {
            if (state.c[0] != setASCII) {
                put(out, escape, '(', 'B');
                state.c[0] = setASCII;
            }
            if (state.c[4] & isShifted) {
                put(out, shiftIn);
                state.c[4] &= ~isShifted;
            }
            put(out, character);
            ++position;
            continue;
        }

        const Designation* chosen = nullptr;
        unsigned code = noCode;
        size_t consumed = 1;
        for (auto& designation : configuration.designations) {
            char32_t characters[2] = { character, 0 };
            int length = 1;
            code = designation.encode(characters, length);
            if (code == dependsOnNext) {
                if (position + 1 == text.length()) {
                    if (!flush)
                        return needsMoreInput;
                    length = -1;
                } else {
                    characters[1] = text[position + 1];
                    length = 2;
                }
                code = designation.encode(characters, length);
                if (code != noCode)
                    consumed = length;
            }
            if (code != noCode) {
                chosen = &designation;
                break;
            }
        }
        if (!chosen)
            return 1;

        uint8_t final = chosen->set & ~twoBytes;
        if (!chosen->plane) {
            if (state.c[4] & isShifted) {
                put(out, shiftIn);
                state.c[4] &= ~isShifted;
            }
            if (state.c[0] != chosen->set) {
                if (chosen->width == 1)
                    put(out, escape, '(', final);
                else if (chosen->set == setJISX0208)
                    put(out, escape, '$', final);
                else
                    put(out, escape, '$', '(', final);
                state.c[0] = chosen->set;
            }
        } else if (chosen->plane == 1) {
            if (state.c[1] != chosen->set) {
                if (chosen->width == 1)
                    put(out, escape, ')', final);
                else
                    put(out, escape, '$', ')', final);
                state.c[1] = chosen->set;
            }
            if (!(state.c[4] & isShifted)) {
                put(out, shiftOut);
                state.c[4] |= isShifted;
            }
        } else
            return internalError;

        if (chosen->width == 1)
            put(out, code);
        else
            putCode(out, code);
        position += consumed;
    }
    return 0;
}

static void startDecodingISO2022(State& state)
{
    state.c[0] = setASCII;
    state.c[1] = setASCII;
    state.c[2] = setASCII;
    state.c[4] = 0;
}

static void resetDecoderISO2022(State& state)
{
    state.c[0] = setASCII;
    state.c[4] &= ~isShifted;
}

static ALWAYS_INLINE bool endsEscapeSequence(unsigned byte) { return (byte >= 'A' && byte <= 'Z') || byte == '@'; }

// At an escape sequence that designates a set.
static int decodeDesignation(const ISO2022& configuration, State& state, std::span<const uint8_t> in, size_t& position)
{
    constexpr size_t longestSequence = 16;
    auto* bytes = in.data() + position;
    size_t left = in.size() - position;
    size_t length = 0;
    for (size_t i = 1; i < longestSequence; ++i) {
        if (i >= left)
            return needsMoreInput;
        if (endsEscapeSequence(bytes[i])) {
            length = i + 1;
            break;
        }
        if (configuration.hasRevisions && i + 1 < left && bytes[i] == '&' && bytes[i + 1] == '@')
            i += 2;
    }

    uint8_t set;
    unsigned plane = 0;
    switch (length) {
    case 0:
        return 1;
    case 3:
        if (bytes[1] == '$')
            set = bytes[2] | twoBytes;
        else {
            set = bytes[2];
            if (bytes[1] == ')')
                plane = 1;
            else if (bytes[1] == '.' && configuration.hasG2)
                plane = 2;
            else if (bytes[1] != '(')
                return 3;
        }
        break;
    case 4:
        if (bytes[1] != '$' || (bytes[2] != '(' && bytes[2] != ')'))
            return 4;
        set = bytes[3] | twoBytes;
        plane = bytes[2] == ')';
        break;
    case 6:
        if (!configuration.hasRevisions || bytes[3] != escape || bytes[4] != '$' || bytes[5] != 'B')
            return 6;
        set = setJISX0208;
        break;
    default:
        return static_cast<int>(length);
    }

    if (set != setASCII) {
        bool isKnown = false;
        for (auto& designation : configuration.designations)
            isKnown |= designation.set == set;
        if (!isKnown)
            return static_cast<int>(length);
    }
    state.c[plane] = set;
    position += length;
    return 0;
}

static int decodeISO2022(const Codec& codec, State& state, std::span<const uint8_t> in, size_t& position, StrBuilder& out)
{
    const ISO2022& configuration = iso2022Of(codec);
    while (position < in.size()) {
        unsigned byte = in[position];
        size_t left = in.size() - position;

        if (state.c[4] & isInUnknownSequence) {
            put(out, byte);
            ++position;
            if (endsEscapeSequence(byte))
                state.c[4] &= ~isInUnknownSequence;
            continue;
        }

        if (byte == escape) {
            if (left < 2)
                return needsMoreInput;
            unsigned second = in[position + 1];
            if (second == '(' || second == ')' || second == '$' || second == '.' || second == '&') {
                if (int result = decodeDesignation(configuration, state, in, position))
                    return result;
            } else if (second == 'N' && configuration.hasG2) {
                // The next byte alone is of G2.
                if (left < 3)
                    return needsMoreInput;
                unsigned third = in[position + 2];
                char32_t character;
                if (state.c[2] == setLatin1)
                    character = third < 0x80 ? third + 0x80 : noCharacter;
                else if (state.c[2] == setGreek)
                    character = iso2022Greek[third];
                else if (state.c[2] == setASCII)
                    character = third < 0x80 ? third : noCharacter;
                else
                    return internalError;
                if (character == noCharacter)
                    return 3;
                put(out, character);
                position += 3;
            } else {
                put(out, escape);
                state.c[4] |= isInUnknownSequence;
                ++position;
            }
            continue;
        }

        if ((byte == shiftIn || byte == shiftOut) && configuration.hasShifts) {
            if (byte == shiftOut)
                state.c[4] |= isShifted;
            else
                state.c[4] &= ~isShifted;
            ++position;
            continue;
        }
        if (byte == '\n')
            state.c[4] &= ~isShifted;
        else if (byte >= 0x80)
            return 1;

        uint8_t set = state.c[4] & isShifted ? state.c[1] : state.c[0];
        if (byte < 0x20 || set == setASCII) {
            put(out, byte);
            ++position;
            continue;
        }
        const Designation* current = nullptr;
        for (auto& designation : configuration.designations) {
            if (designation.set == set) {
                current = &designation;
                break;
            }
        }
        // setstate() may have been given anything.
        if (!current)
            return 1;
        if (left < current->width)
            return needsMoreInput;
        char32_t character = current->decode(in.data() + position);
        if (character == noCharacter)
            return current->width;
        if (character < 0x30000)
            put(out, character);
        else {
            put(out, character >> 16);
            put(out, character & 0xFFFF);
        }
        position += current->width;
    }
    return 0;
}

static char32_t iso2022DecodeKSX1001(const uint8_t* bytes) { return ksx1001.character(bytes[0], bytes[1]); }
static unsigned iso2022EncodeKSX1001(const char32_t* characters, int&) { return ksx1001.code(*characters); }
static char32_t iso2022DecodeGB2312(const uint8_t* bytes) { return gb2312.character(bytes[0], bytes[1]); }
static unsigned iso2022EncodeGB2312(const char32_t* characters, int&) { return gb2312.code(*characters); }
static char32_t iso2022DecodeJISX0212(const uint8_t* bytes) { return jisx0212.character(bytes[0], bytes[1]); }
static unsigned iso2022EncodeJISX0212(const char32_t* characters, int&) { return jisx0212.code(*characters); }

static char32_t iso2022DecodeJISX0208(const uint8_t* bytes)
{
    return (bytes[0] << 8 | bytes[1]) == codeOfBackslash ? fullwidthBackslash : jisx0208.character(bytes[0], bytes[1]);
}

static unsigned iso2022EncodeJISX0208(const char32_t* characters, int&)
{
    return *characters == fullwidthBackslash ? codeOfBackslash : jisx0208.code(*characters);
}

static char32_t iso2022DecodeRoman(const uint8_t* bytes) { return *bytes == 0x5C ? yenSign : *bytes == 0x7E ? overline : *bytes; }
static unsigned iso2022EncodeRoman(const char32_t* characters, int&) { return *characters == yenSign ? 0x5C : *characters == overline ? 0x7E : noCode; }
static char32_t iso2022DecodeKatakana(const uint8_t* bytes) { return isKatakanaByte(*bytes | 0x80) ? (*bytes | 0x80) + katakanaOffset : noCharacter; }
static unsigned iso2022EncodeKatakana(const char32_t* characters, int&) { return isHalfwidthKatakana(*characters) ? *characters - katakanaOffset - 0x80 : noCode; }

// These two are only ever in G2, which the decoder sees to itself. Nothing is encoded with them.
static char32_t iso2022DecodeNothing(const uint8_t*) { return noCharacter; }
static unsigned iso2022EncodeNothing(const char32_t*, int&) { return noCode; }

template<bool is2000>
static char32_t iso2022DecodeJISX0213Plane1(const uint8_t* bytes)
{
    unsigned code = bytes[0] << 8 | bytes[1];
    if (is2000 && wasAddedToPlane1In2004(code))
        return noCharacter;
    if (code == codeOfBackslash)
        return fullwidthBackslash;
    char32_t second = 0;
    char32_t character = jisx0213Plane1Character(bytes[0], bytes[1], second);
    return second ? character << 16 | second : character;
}

// (Of either edition: here CPython's does not have U+9B1D where the edition of 2000 had it.)
static char32_t iso2022DecodeJISX0213Plane2(const uint8_t* bytes) { return jisx0213Plane2.character(bytes[0], bytes[1]); }

template<bool is2000>
static unsigned iso2022JISX0213Code(const char32_t* characters, int& length)
{
    if (length == 1) {
        unsigned code = jisx0213Code(*characters, is2000);
        if (code != noCode || (is2000 && wasAddedIn2004(*characters)))
            return code;
        return jisx0208.code(*characters);
    }
    if (length == 2 && characters[1]) {
        if (unsigned code = jisx0213CodeOfPair(characters[0], characters[1]); code != noCode)
            return code;
    }
    length = 1;
    return jisx0213CodeAlone(*characters);
}

template<bool is2000, bool wantsPlane2>
static unsigned iso2022EncodeJISX0213(const char32_t* characters, int& length)
{
    unsigned code = iso2022JISX0213Code<is2000>(characters, length);
    if (code == noCode || code == dependsOnNext)
        return code;
    if (!!(code & inPlane2) != wantsPlane2)
        return noCode;
    return code & ~inPlane2;
}

// Comes before JIS X 0208 in the list, to catch the pairs whose first character that set has.
template<bool is2000>
static unsigned iso2022EncodeJISX0213Pair(const char32_t* characters, int& length)
{
    int given = length;
    unsigned code = iso2022JISX0213Code<is2000>(characters, length);
    if (given == 1)
        return code == dependsOnNext ? dependsOnNext : noCode;
    return given == 2 && length == 2 ? code : noCode;
}

constexpr Designation designationJISX0208 { setJISX0208, 0, 2, iso2022DecodeJISX0208, iso2022EncodeJISX0208 };
constexpr Designation designationJISX0208Of1978 { setJISX0208Of1978, 0, 2, iso2022DecodeJISX0208, iso2022EncodeJISX0208 };
constexpr Designation designationJISX0212 { setJISX0212, 0, 2, iso2022DecodeJISX0212, iso2022EncodeJISX0212 };
constexpr Designation designationRoman { setRoman, 0, 1, iso2022DecodeRoman, iso2022EncodeRoman };
constexpr Designation designationKatakana { setKatakana, 0, 1, iso2022DecodeKatakana, iso2022EncodeKatakana };

static constexpr Designation designationsKR[] = {
    { setKSX1001, 1, 2, iso2022DecodeKSX1001, iso2022EncodeKSX1001 },
};
static constexpr Designation designationsJP[] = { designationJISX0208, designationRoman, designationJISX0208Of1978 };
static constexpr Designation designationsJP1[] = { designationJISX0208, designationJISX0212, designationRoman, designationJISX0208Of1978 };
static constexpr Designation designationsJP2[] = {
    designationJISX0208,
    designationJISX0212,
    { setKSX1001, 0, 2, iso2022DecodeKSX1001, iso2022EncodeKSX1001 },
    { setGB2312, 0, 2, iso2022DecodeGB2312, iso2022EncodeGB2312 },
    designationRoman,
    designationJISX0208Of1978,
    { setLatin1, 2, 1, iso2022DecodeNothing, iso2022EncodeNothing },
    { setGreek, 2, 1, iso2022DecodeNothing, iso2022EncodeNothing },
};
static constexpr Designation designationsJP2004[] = {
    { setJISX0213Plane1Of2004, 0, 2, iso2022DecodeJISX0213Plane1<false>, iso2022EncodeJISX0213Pair<false> },
    designationJISX0208,
    { setJISX0213Plane1Of2004, 0, 2, iso2022DecodeJISX0213Plane1<false>, iso2022EncodeJISX0213<false, false> },
    { setJISX0213Plane2, 0, 2, iso2022DecodeJISX0213Plane2, iso2022EncodeJISX0213<false, true> },
};
static constexpr Designation designationsJP3[] = {
    { setJISX0213Plane1Of2000, 0, 2, iso2022DecodeJISX0213Plane1<true>, iso2022EncodeJISX0213Pair<true> },
    designationJISX0208,
    { setJISX0213Plane1Of2000, 0, 2, iso2022DecodeJISX0213Plane1<true>, iso2022EncodeJISX0213<true, false> },
    { setJISX0213Plane2, 0, 2, iso2022DecodeJISX0213Plane2, iso2022EncodeJISX0213<true, true> },
};
static constexpr Designation designationsJPExt[] = { designationJISX0208, designationJISX0212, designationRoman, designationKatakana, designationJISX0208Of1978 };

static constexpr ISO2022 iso2022KR { true, false, false, designationsKR };
static constexpr ISO2022 iso2022JP { false, false, true, designationsJP };
static constexpr ISO2022 iso2022JP1 { false, false, true, designationsJP1 };
static constexpr ISO2022 iso2022JP2 { false, true, true, designationsJP2 };
static constexpr ISO2022 iso2022JP2004 { false, false, true, designationsJP2004 };
static constexpr ISO2022 iso2022JP3 { false, false, true, designationsJP3 };
static constexpr ISO2022 iso2022JPExt { false, false, true, designationsJPExt };

// ---- The lists ----

static constexpr Codec stateless(ASCIILiteral name, decltype(Codec::encode) encode, decltype(Codec::decode) decode, const void* configuration = nullptr)
{
    return { name, encode, decode, nullptr, nullptr, nullptr, nullptr, configuration };
}

static constexpr Codec iso2022(ASCIILiteral name, const ISO2022& configuration)
{
    return { name, encodeISO2022, decodeISO2022, startEncodingISO2022, finishEncodingISO2022, startDecodingISO2022, resetDecoderISO2022, &configuration };
}

static constexpr Codec codecsCN[] = {
    stateless("gb2312"_s, encodeDoubleByte<gb2312Code>, decodeDoubleByte<gb2312Character>),
    stateless("gbk"_s, encodeDoubleByte<gbkCode>, decodeDoubleByte<gbkCharacter>),
    stateless("gb18030"_s, encodeGB18030, decodeGB18030),
    { "hz"_s, encodeHZ, decodeHZ, startHZ, finishEncodingHZ, startHZ, startHZ, nullptr },
};

static constexpr Codec codecsHK[] = {
    stateless("big5hkscs"_s, encodeBig5HKSCS, decodeBig5HKSCS),
};

static constexpr Codec codecsISO2022[] = {
    iso2022("iso2022_kr"_s, iso2022KR),
    iso2022("iso2022_jp"_s, iso2022JP),
    iso2022("iso2022_jp_1"_s, iso2022JP1),
    iso2022("iso2022_jp_2"_s, iso2022JP2),
    iso2022("iso2022_jp_2004"_s, iso2022JP2004),
    iso2022("iso2022_jp_3"_s, iso2022JP3),
    iso2022("iso2022_jp_ext"_s, iso2022JPExt),
};

static constexpr Codec codecsJP[] = {
    stateless("shift_jis"_s, encodeShiftJIS, decodeShiftJIS),
    stateless("cp932"_s, encodeCP932, decodeCP932),
    stateless("euc_jp"_s, encodeEUCJP, decodeEUCJP),
    stateless("shift_jis_2004"_s, encodeShiftJIS2004, decodeShiftJIS2004),
    stateless("euc_jis_2004"_s, encodeEUCJIS2004, decodeEUCJIS2004),
    stateless("euc_jisx0213"_s, encodeEUCJIS2004, decodeEUCJIS2004, &edition2000),
    stateless("shift_jisx0213"_s, encodeShiftJIS2004, decodeShiftJIS2004, &edition2000),
};

static constexpr Codec codecsKR[] = {
    stateless("euc_kr"_s, encodeEUCKR, decodeEUCKR),
    stateless("cp949"_s, encodeDoubleByte<cp949Code>, decodeDoubleByte<cp949Character>),
    stateless("johab"_s, encodeDoubleByte<johabCodeOf>, decodeDoubleByte<johabCharacter>),
};

static constexpr Codec codecsTW[] = {
    stateless("big5"_s, encodeDoubleByte<big5Code>, decodeDoubleByte<big5Character>),
    stateless("cp950"_s, encodeDoubleByte<cp950Code>, decodeDoubleByte<cp950Character>),
};

// ---- The modules ----

template<const auto& codecs>
static JSValue getCodec(JSGlobalObject* globalObject, JSValue, JSValue encoding)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!isStr(encoding))
        return raiseTypeError(globalObject, scope, "encoding name must be a string."_s);
    String name = strValue(encoding)->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    if (!name.is8Bit()) {
        // A lone surrogate is a UnicodeEncodeError.
        codecEncodeUTF8(globalObject, strValue(encoding), String());
        RETURN_IF_EXCEPTION(scope, { });
    }
    // CPython compares C strings.
    StringView compared = name;
    if (size_t end = compared.find(static_cast<char16_t>(0)); end != notFound)
        compared = compared.left(end);
    for (const Codec& codec : codecs) {
        if (compared == codec.name)
            RELEASE_AND_RETURN(scope, makeMultibyteCodec(globalObject, codec));
    }
    return raiseLookupError(globalObject, scope, "no such codec is supported."_s);
}

} // namespace CJK

JSValue makeCapsule(JSGlobalObject*, ASCIILiteral name); // PyTypesModule.cpp

// In CPython the modules get at one another's tables through these. Here they are only there.
static void addCJKMaps(JSGlobalObject* globalObject, PyModule* module, std::initializer_list<ASCIILiteral> names)
{
    for (ASCIILiteral name : names)
        module->add(globalObject, name, makeCapsule(globalObject, "multibytecodec.map"_s));
}

#define PY_DEFINE_CJK_MODULE(Name, codecs, ...) \
    static constexpr NativeDef functionsOf##Name[] = { \
        PY_FUNCTION("getcodec", CJK::getCodec<CJK::codecs>, "($self, object, /)"), \
    }; \
    void initialize##Name##Module(JSGlobalObject* globalObject, PyModule* module) \
    { \
        module->addFunctions(globalObject, functionsOf##Name); \
        addCJKMaps(globalObject, module, { __VA_ARGS__ }); \
    }

PY_DEFINE_CJK_MODULE(CodecsCN, codecsCN, "__map_gb2312"_s, "__map_gbkext"_s, "__map_gbcommon"_s, "__map_gb18030ext"_s)
PY_DEFINE_CJK_MODULE(CodecsHK, codecsHK, "__map_big5hkscs"_s, "__map_big5hkscs_bmp"_s, "__map_big5hkscs_nonbmp"_s)
PY_DEFINE_CJK_MODULE(CodecsISO2022, codecsISO2022)
PY_DEFINE_CJK_MODULE(CodecsJP, codecsJP, "__map_jisx0208"_s, "__map_jisx0212"_s, "__map_jisxcommon"_s, "__map_jisx0213_1_bmp"_s, "__map_jisx0213_2_bmp"_s, "__map_jisx0213_bmp"_s, "__map_jisx0213_1_emp"_s, "__map_jisx0213_2_emp"_s, "__map_jisx0213_emp"_s, "__map_jisx0213_pair"_s, "__map_cp932ext"_s)
PY_DEFINE_CJK_MODULE(CodecsKR, codecsKR, "__map_ksx1001"_s, "__map_cp949"_s, "__map_cp949ext"_s)
PY_DEFINE_CJK_MODULE(CodecsTW, codecsTW, "__map_big5"_s, "__map_cp950ext"_s)

#undef PY_DEFINE_CJK_MODULE

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
