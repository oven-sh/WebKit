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


#include "config.h"
#include "PythonUnicodeData.h"

#include "JSCInlines.h"
#include "PyObjects.h"
#include "PyRealm.h"
#include "PyStateObject.h"
#include "PythonBuiltins.h"
#include "PythonBytes.h"
#include "PythonCodecs.h"
#include "PythonNumbers.h"
#include "PythonOperations.h"
#include "PythonText.h"
#include "PythonUnicodeDatabase.h"
#include "PythonUnicodeNames.h"
#include "PythonUnicodeType.h"

// The module unicodedata: Modules/unicodedata.c of CPython.

namespace JSC { namespace Python {

namespace {

using namespace UnicodeDatabase;

// An earlier version of the database, as what is different in it
struct PreviousVersion {
    ASCIILiteral name;
    const ChangeRecord* (*record)(char32_t);
    char32_t (*normalization)(char32_t);
};

constexpr PreviousVersion version_3_2_0 { "3.2.0"_s, changeRecord_3_2_0, normalization_3_2_0 };

// _getrecord_ex()
const DatabaseRecord& recordOf(char32_t code)
{
    unsigned index = 0;
    if (code < 0x110000) {
        index = index1[code >> shift];
        index = index2[(index << shift) + (code & ((1 << shift) - 1))];
    }
    return databaseRecords[index];
}

// For taking Hangul syllables apart and putting them together
constexpr char32_t sBase = 0xAC00;
constexpr char32_t lBase = 0x1100;
constexpr char32_t vBase = 0x1161;
constexpr char32_t tBase = 0x11A7;
constexpr unsigned lCount = 19;
constexpr unsigned vCount = 21;
constexpr unsigned tCount = 28;
constexpr unsigned nCount = vCount * tCount;
constexpr unsigned sCount = lCount * nCount;

bool isHangulSyllable(char32_t code) { return sBase <= code && code < sBase + sCount; }

struct Decomposition {
    unsigned index; // Where in decompositionData the first character is
    unsigned prefix; // Which of decompositionPrefixes. 0 is that it is canonical.
    unsigned count;
};

// get_decomp_record()
Decomposition decompositionOf(const PreviousVersion* previous, char32_t code)
{
    unsigned index = 0;
    if (code < 0x110000 && !(previous && !previous->record(code)->categoryChanged)) {
        index = decompositionIndex1[code >> decompositionShift];
        index = decompositionIndex2[(index << decompositionShift) + (code & ((1 << decompositionShift) - 1))];
    }
    // The high byte is how many there are, and the low byte the prefix.
    return { index + 1, decompositionData[index] & 255, decompositionData[index] >> 8 };
}

// ---- The normal forms

// nfd_nfkd(). False if there is no room.
bool tryDecompose(const PreviousVersion* previous, std::span<const char32_t> input, bool isCompatibility, Vector<char32_t>& output)
{
    if (!output.tryReserveCapacity(input.size() + std::min<size_t>(input.size(), 10)))
        return false;
    Vector<char32_t, 20> stack;
    for (char32_t character : input) {
        stack.append(character);
        while (!stack.isEmpty()) {
            char32_t code = stack.takeLast();
            if (output.capacity() - output.size() < 3 && !output.tryReserveCapacity(output.size() + std::max<size_t>(output.size() / 2, 16)))
                return false;
            if (isHangulSyllable(code)) {
                unsigned sIndex = code - sBase;
                output.append(lBase + sIndex / nCount);
                output.append(vBase + (sIndex % nCount) / tCount);
                if (sIndex % tCount)
                    output.append(tBase + sIndex % tCount);
                continue;
            }
            if (previous) {
                if (char32_t value = previous->normalization(code)) {
                    stack.append(value);
                    continue;
                }
            }
            auto decomposition = decompositionOf(previous, code);
            // It stays as it is if it does not come apart, or does so only for compatibility and that is not what is wanted.
            if (!decomposition.count || (decomposition.prefix && !isCompatibility)) {
                output.append(code);
                continue;
            }
            for (unsigned count = decomposition.count; count;)
                stack.append(decompositionData[decomposition.index + --count]);
        }
    }

    // Each run of combining characters is put in order of their classes, those of the same class staying as they were. CPython has two ways of sorting, for a short run and a long one, that come to the same.
    auto characters = output.mutableSpan();
    for (size_t i = 0; i < characters.size();) {
        if (!recordOf(characters[i]).combining) {
            ++i;
            continue;
        }
        size_t start = i;
        while (i < characters.size() && recordOf(characters[i]).combining)
            ++i;
        std::ranges::stable_sort(characters.subspan(start, i - start), { }, [] (char32_t c) { return recordOf(c).combining; });
    }
    return true;
}

// find_nfc_index(). -1 if it is not there.
int findCompositionIndex(const Reindex* table, char32_t code)
{
    for (unsigned index = 0; table[index].start; ++index) {
        unsigned start = table[index].start;
        if (code < start)
            return -1;
        if (code <= start + table[index].count)
            return table[index].index + static_cast<int>(code - start);
    }
    return -1;
}

// nfc_nfkc(). False if there is no room.
bool tryCompose(const PreviousVersion* previous, std::span<const char32_t> input, bool isCompatibility, Vector<char32_t>& output)
{
    Vector<char32_t> decomposed;
    if (!tryDecompose(previous, input, isCompatibility, decomposed) || !output.tryReserveCapacity(decomposed.size()))
        return false;
    auto data = decomposed.span();
    size_t length = data.size();
    // Where there are characters that have been taken into one before them. CPython has room for 20.
    Vector<size_t, 20> skipped;
    for (size_t i = 0; i < length;) {
        if (size_t found = skipped.find(i); found != notFound) {
            skipped[found] = skipped.last();
            skipped.removeLast();
            ++i;
            continue;
        }
        // It has all been taken apart, so there is no syllable with something to be added to it.
        char32_t code = data[i];
        if (lBase <= code && code < lBase + lCount && i + 1 < length && vBase <= data[i + 1] && data[i + 1] < vBase + vCount) {
            code = sBase + ((code - lBase) * vCount + (data[i + 1] - vBase)) * tCount;
            i += 2;
            if (i < length && tBase < data[i] && data[i] < tBase + tCount) {
                code += data[i] - tBase;
                ++i;
            }
            output.append(code);
            continue;
        }
        int first = findCompositionIndex(nfcFirst, code);
        if (first == -1) {
            output.append(code);
            ++i;
            continue;
        }
        // It goes with the next that nothing comes between, and what that comes to with the next again.
        char32_t composed = code;
        int combining = 0;
        for (size_t next = i + 1; next < length; ++next) {
            char32_t other = data[next];
            int otherCombining = recordOf(other).combining;
            if (combining) {
                if (!otherCombining)
                    break;
                if (combining >= otherCombining)
                    continue; // Something comes between.
            }
            int last = findCompositionIndex(nfcLast, other);
            char32_t together = 0;
            if (last != -1) {
                unsigned index = static_cast<unsigned>(first) * totalLast + static_cast<unsigned>(last);
                together = compositionData[(compositionIndex[index >> compositionShift] << compositionShift) + (index & ((1 << compositionShift) - 1))];
            }
            if (!together) {
                // They do not go together. If it begins something of its own there is no looking further.
                if (!otherCombining)
                    break;
                combining = otherCombining;
                continue;
            }
            composed = together;
            skipped.append(next);
            first = findCompositionIndex(nfcFirst, composed);
            if (first == -1)
                break;
        }
        output.append(composed);
        ++i;
    }
    return true;
}

enum class QuickCheck : uint8_t { Yes, Maybe, No };

// is_normalized_quickcheck()
QuickCheck quickCheck(const PreviousVersion* previous, std::span<const char32_t> input, Unicode::NormalizationForm form, bool yesOnly)
{
    // There is nothing written down to go by for an earlier version.
    if (previous)
        return QuickCheck::Maybe;
    bool isComposed = form == Unicode::NormalizationForm::NFC || form == Unicode::NormalizationForm::NFKC;
    bool isCompatibility = form == Unicode::NormalizationForm::NFKD || form == Unicode::NormalizationForm::NFKC;
    unsigned quickCheckShift = (isComposed ? 4 : 0) + (isCompatibility ? 2 : 0);
    QuickCheck result = QuickCheck::Yes;
    unsigned char previousCombining = 0;
    for (char32_t character : input) {
        auto& record = recordOf(character);
        if (record.combining && previousCombining > record.combining)
            return QuickCheck::No;
        previousCombining = record.combining;
        unsigned bits = (record.normalizationQuickCheck >> quickCheckShift) & 3;
        if (yesOnly) {
            if (bits)
                return QuickCheck::Maybe;
        } else if (bits == static_cast<unsigned>(QuickCheck::No))
            return QuickCheck::No;
        else if (bits == static_cast<unsigned>(QuickCheck::Maybe))
            result = QuickCheck::Maybe;
    }
    return result;
}

bool tryNormalize(const PreviousVersion* previous, Unicode::NormalizationForm form, std::span<const char32_t> input, Vector<char32_t>& result)
{
    switch (form) {
    case Unicode::NormalizationForm::NFD:
        return tryDecompose(previous, input, false, result);
    case Unicode::NormalizationForm::NFKD:
        return tryDecompose(previous, input, true, result);
    case Unicode::NormalizationForm::NFC:
        return tryCompose(previous, input, false, result);
    case Unicode::NormalizationForm::NFKC:
        return tryCompose(previous, input, true, result);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// ---- Names

constexpr const char* hangulSyllables[][3] = {
    { "G", "A", "" },
    { "GG", "AE", "G" },
    { "N", "YA", "GG" },
    { "D", "YAE", "GS" },
    { "DD", "EO", "N" },
    { "R", "E", "NJ" },
    { "M", "YEO", "NH" },
    { "B", "YE", "D" },
    { "BB", "O", "L" },
    { "S", "WA", "LG" },
    { "SS", "WAE", "LM" },
    { "", "OE", "LB" },
    { "J", "YO", "LS" },
    { "JJ", "U", "LT" },
    { "C", "WEO", "LP" },
    { "K", "WE", "LH" },
    { "T", "WI", "M" },
    { "P", "YU", "B" },
    { "H", "EU", "BS" },
    { nullptr, "YI", "S" },
    { nullptr, "I", "SS" },
    { nullptr, nullptr, "NG" },
    { nullptr, nullptr, "J" },
    { nullptr, nullptr, "C" },
    { nullptr, nullptr, "K" },
    { nullptr, nullptr, "T" },
    { nullptr, nullptr, "P" },
    { nullptr, nullptr, "H" },
};

// find_prefix_id(): which of derivedNamePrefixes the name of a character begins with, if its name is worked out and not looked up. -1 if it is not.
int findPrefix(char32_t code)
{
    for (auto& range : derivedNameRanges) {
        if (code < range.first)
            return -1;
        if (code <= range.last)
            return range.prefix;
    }
    return -1;
}

// Aliases, and the names of sequences, are kept as the names of characters that are for private use.
bool isAlias(char32_t code) { return code >= aliasesStart && code < aliasesEnd; }
bool isNamedSequence(char32_t code) { return code >= namedSequencesStart && code < namedSequencesEnd; }

// The names are all in one graph, in which those that begin alike or end alike have that much between them. A node is a number, of which the lowest bit is whether a name ends there and the rest is how many names there are from
// there on, and then its edges. Which name it is, in order, is what the number of a name is, and there is a table from that to the character and another the other way.
//
// An edge is a number, of which the lowest bit is whether it is the last of the node's, and the next whether it has one letter. The rest is how much further on the node that it goes to is than that of the edge before, or, for the
// first, than the edge itself. Then comes how many letters, if not one, and the letters. A node where a name ends that has no edges is followed by a 0.

// _dawg_decode_varint_unsigned(): where what follows it is
unsigned decodeVarint(unsigned index, unsigned& result)
{
    result = 0;
    for (unsigned shiftBy = 0;; shiftBy += 7) {
        unsigned char byte = packedNameDawg[index++];
        result |= (byte & 0x7f) << shiftBy;
        if (!(byte & 0x80))
            return index;
    }
}

// _dawg_decode_node(): where its edges are
unsigned decodeNode(unsigned nodeOffset, bool& isFinal)
{
    unsigned number;
    nodeOffset = decodeVarint(nodeOffset, number);
    isFinal = number & 1;
    return nodeOffset;
}

bool isFinalNode(unsigned nodeOffset)
{
    bool isFinal;
    decodeNode(nodeOffset, isFinal);
    return isFinal;
}

unsigned descendantCount(unsigned nodeOffset)
{
    unsigned number;
    decodeVarint(nodeOffset, number);
    return number >> 1;
}

struct Edge {
    unsigned size;
    unsigned labelOffset;
    unsigned targetNodeOffset;
    bool isLast;
};

// _dawg_decode_edge(). Nothing if there are no edges.
std::optional<Edge> decodeEdge(bool isFirstEdge, unsigned previousTargetNodeOffset, unsigned edgeOffset)
{
    unsigned number;
    edgeOffset = decodeVarint(edgeOffset, number);
    if (!number && isFirstEdge)
        return std::nullopt;
    Edge edge;
    edge.isLast = number & 1;
    bool hasOneLetter = number & 2;
    edge.targetNodeOffset = previousTargetNodeOffset + (number >> 2);
    edge.size = hasOneLetter ? 1 : packedNameDawg[edgeOffset++];
    edge.labelOffset = edgeOffset;
    return edge;
}

// _lookup_dawg_packed(): which name it is. Nothing if it is none.
std::optional<unsigned> lookUpName(std::span<const char> name)
{
    unsigned position = 0;
    unsigned nodeOffset = 0;
    unsigned result = 0; // How many names have been passed over
    while (position < name.size()) {
        bool isFinal;
        unsigned edgeOffset = decodeNode(nodeOffset, isFinal);
        unsigned previousTargetNodeOffset = edgeOffset;
        for (bool isFirstEdge = true;; isFirstEdge = false) {
            auto edge = decodeEdge(isFirstEdge, previousTargetNodeOffset, edgeOffset);
            if (!edge)
                return std::nullopt;
            previousTargetNodeOffset = edge->targetNodeOffset;
            // _dawg_match_edge()
            bool matches = position + edge->size <= name.size();
            for (unsigned i = 0; matches && i < edge->size; ++i) {
                if (packedNameDawg[edge->labelOffset + i] != static_cast<unsigned char>(toASCIIUpper(name[position + i]))) {
                    // No other edge begins with the same letter.
                    if (i)
                        return std::nullopt;
                    matches = false;
                }
            }
            if (matches) {
                result += isFinal;
                position += edge->size;
                nodeOffset = edge->targetNodeOffset;
                break;
            }
            if (edge->isLast)
                return std::nullopt;
            result += descendantCount(edge->targetNodeOffset);
            edgeOffset = edge->labelOffset + edge->size;
        }
    }
    if (!isFinalNode(nodeOffset))
        return std::nullopt;
    return result;
}

using NameBuffer = Vector<char, nameMaxLength + 1>;

// _inverse_dawg_lookup(): the name that is such and such a one. False if there is none, or it is longer than any is to be.
bool nameAt(unsigned position, NameBuffer& buffer)
{
    unsigned nodeOffset = 0;
    for (;;) {
        bool isFinal;
        unsigned edgeOffset = decodeNode(nodeOffset, isFinal);
        if (isFinal) {
            if (!position)
                return buffer.size() + 1 != nameMaxLength;
            --position;
        }
        unsigned previousTargetNodeOffset = edgeOffset;
        for (bool isFirstEdge = true;; isFirstEdge = false) {
            auto edge = decodeEdge(isFirstEdge, previousTargetNodeOffset, edgeOffset);
            if (!edge)
                return false;
            previousTargetNodeOffset = edge->targetNodeOffset;
            unsigned count = descendantCount(edge->targetNodeOffset);
            if (position < count) {
                if (buffer.size() + edge->size >= nameMaxLength)
                    return false;
                for (unsigned i = 0; i < edge->size; ++i)
                    buffer.append(static_cast<char>(packedNameDawg[edge->labelOffset + i]));
                nodeOffset = edge->targetNodeOffset;
                break;
            }
            if (edge->isLast)
                return false;
            position -= count;
            edgeOffset = edge->labelOffset + edge->size;
        }
    }
}

// _getucname(). False if it has none.
bool nameOf(const PreviousVersion* previous, char32_t code, bool withAliasesAndSequences, NameBuffer& buffer)
{
    if (code >= 0x110000)
        return false;
    if (!withAliasesAndSequences && (isAlias(code) || isNamedSequence(code)))
        return false;
    if (previous) {
        // There were no aliases then, or names for sequences.
        if (isAlias(code) || isNamedSequence(code))
            return false;
        if (!previous->record(code)->categoryChanged)
            return false;
    }
    auto append = [&] (const char* text) { buffer.append(unsafeSpan(text)); };
    int prefix = findPrefix(code);
    if (!prefix) {
        unsigned sIndex = code - sBase;
        append("HANGUL SYLLABLE ");
        append(hangulSyllables[sIndex / nCount][0]);
        append(hangulSyllables[(sIndex % nCount) / tCount][1]);
        append(hangulSyllables[sIndex % tCount][2]);
        return true;
    }
    // Only for the CJK unified ideographs: the Tangut ones have theirs from CPython 3.15.
    if (prefix == 1) {
        buffer.append(derivedNamePrefixes[prefix].span());
        auto digits = hex(static_cast<unsigned>(code), 4);
        buffer.append(byteCast<char>(digits.span()));
        return true;
    }
    unsigned offset = dawgCodePointToPositionIndex1[code >> dawgCodePointToPositionShift];
    offset = dawgCodePointToPositionIndex2[(offset << dawgCodePointToPositionShift) + (code & ((1 << dawgCodePointToPositionShift) - 1))];
    if (offset == dawgCodePointToPositionNotFound)
        return false;
    return nameAt(offset, buffer);
}

// PyOS_strnicmp() == 0, of text that ends with a zero
bool beginsWithIgnoringCase(const char* text, const char* prefix)
{
    for (; *prefix; ++text, ++prefix) {
        if (toASCIILower(*text) != toASCIILower(*prefix))
            return false;
    }
    return true;
}

// find_syllable(): the longest in a column that the text begins with
void findSyllable(const char* text, int& length, int& position, unsigned count, unsigned column)
{
    length = -1;
    for (unsigned i = 0; i < count; ++i) {
        const char* syllable = hangulSyllables[i][column];
        int syllableLength = static_cast<int>(strlen(syllable));
        if (syllableLength <= length)
            continue;
        if (beginsWithIgnoringCase(text, syllable)) {
            length = syllableLength;
            position = static_cast<int>(i);
        }
    }
    if (length == -1)
        length = 0;
}

// parse_hex_code(). Nothing if it is not one.
std::optional<char32_t> parseHexCode(std::span<const char> digits)
{
    if (digits.size() < 4 || digits.size() > 6 || digits[0] == '0')
        return std::nullopt;
    unsigned value = 0;
    for (char c : digits) {
        if (!isASCIIHexDigit(c))
            return std::nullopt;
        value = value * 16 + toASCIIHexValue(c);
    }
    if (value > 0x10ffff)
        return std::nullopt;
    return value;
}

// _getcode(): the character that has a name. An alias, or the name of a sequence, gives the character for private use that it is kept under. `name` has a zero after it.
std::optional<char32_t> codeNamed(std::span<const char> name)
{
    unsigned which = 0;
    for (; which < std::size(derivedNamePrefixes); ++which) {
        if (beginsWithIgnoringCase(name.data(), derivedNamePrefixes[which].characters()))
            break;
    }
    if (!which) {
        int length;
        int l = -1;
        int v = -1;
        int t = -1;
        const char* position = name.data() + 16;
        findSyllable(position, length, l, lCount, 0);
        position += length;
        findSyllable(position, length, v, vCount, 1);
        position += length;
        findSyllable(position, length, t, tCount, 2);
        position += length;
        if (l != -1 && v != -1 && t != -1 && static_cast<size_t>(position - name.data()) == name.size())
            return sBase + (static_cast<unsigned>(l) * vCount + static_cast<unsigned>(v)) * tCount + static_cast<unsigned>(t);
        return std::nullopt;
    }
    if (which < std::size(derivedNamePrefixes)) {
        auto value = parseHexCode(name.subspan(derivedNamePrefixes[which].length()));
        if (!value || findPrefix(*value) != static_cast<int>(which))
            return std::nullopt;
        return value;
    }
    auto position = lookUpName(name);
    if (!position)
        return std::nullopt;
    return dawgPositionToCodePoint[*position];
}

} // anonymous namespace

namespace Unicode {

bool isCertainlyNormalized(NormalizationForm form, std::span<const char32_t> characters) { return quickCheck(nullptr, characters, form, true) == QuickCheck::Yes; }
bool tryNormalize(NormalizationForm form, std::span<const char32_t> characters, Vector<char32_t>& result) { return Python::tryNormalize(nullptr, form, characters, result); }

bool isWide(char32_t character)
{
    ASCIILiteral width = eastAsianWidthNames[recordOf(character).eastAsianWidth];
    return width == "F"_s || width == "W"_s;
}

String nameOfCharacter(char32_t character, bool withAliasesAndSequences)
{
    NameBuffer buffer;
    if (!nameOf(nullptr, character, withAliasesAndSequences, buffer))
        return { };
    return String(byteCast<Latin1Character>(buffer.span()));
}

std::optional<char32_t> characterNamed(std::span<const uint8_t> name)
{
    Vector<char, 64> text;
    text.append(byteCast<char>(name));
    text.append('\0');
    auto code = codeNamed(text.span().first(name.size()));
    // _check_alias_and_seq()
    if (!code || isNamedSequence(*code))
        return std::nullopt;
    if (isAlias(*code))
        return nameAliases[*code - aliasesStart];
    return code;
}

} // namespace Unicode

// ---- The module

namespace {

struct UnicodeDataModuleState final : NativeState {
    PYTHON_NATIVE_STATE(UnicodeDataModuleState);
    WriteBarrier<PyType> ucd;
    WriteBarrier<Unknown> ucd_3_2_0;
};

template<typename Visitor>
void UnicodeDataModuleState::visit(Visitor& visitor)
{
    visitor.append(ucd);
    visitor.append(ucd_3_2_0);
}

// unicodedata.UCD
struct PreviousVersionState final : NativeState {
    PYTHON_NATIVE_STATE(PreviousVersionState);
    explicit PreviousVersionState(const PreviousVersion& version)
        : version(version)
    {
    }
    const PreviousVersion& version;
};

template<typename Visitor> void PreviousVersionState::visit(Visitor&) { }

// `int(accept={str})` of Argument Clinic. Nothing if it raised. `argument` is "argument" or "argument 1".
std::optional<char32_t> toCharacterArgument(JSGlobalObject* globalObject, JSValue value, ASCIILiteral function, ASCIILiteral argument)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    JSString* string = stringIn(value);
    if (!string) {
        raiseTypeError(globalObject, scope, concatenate(function, "() "_s, argument, " must be a unicode character, not "_s, typeNameOfArgument(globalObject, value)));
        return std::nullopt;
    }
    auto view = string->view(globalObject);
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    size_t length = 0;
    char32_t first = 0;
    for (char32_t character : view->codePoints()) {
        if (!length++)
            first = character;
    }
    if (length != 1) {
        raiseTypeError(globalObject, scope, concatenate(function, "(): "_s, argument, " must be a unicode character, not a string of length "_s, length));
        return std::nullopt;
    }
    return first;
}

// The functions of the module are the methods of UCD as well. `self` is the module for the one, and is left out here, and an earlier version of the database for the other.
struct UCDCall {
    const PreviousVersion* previous;
    unsigned first; // Which argument is the first after self
};

UCDCall callOf(CallFrame* callFrame)
{
    if (!unpack<bool>(callFrame, 0))
        return { nullptr, 0 };
    return { &stateOf<PreviousVersionState>(callFrame->uncheckedArgument(0)).version, 1 };
}

JSValue strOfCharacters(JSGlobalObject* globalObject, std::span<const char32_t> characters)
{
    TextBuilder builder;
    for (char32_t character : characters)
        builder.append(character);
    return strOrMemoryError(globalObject, builder.toString());
}

std::optional<Unicode::NormalizationForm> toForm(JSGlobalObject* globalObject, JSString* form)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    auto view = form->view(globalObject);
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    if (view == "NFC"_s)
        return Unicode::NormalizationForm::NFC;
    if (view == "NFKC"_s)
        return Unicode::NormalizationForm::NFKC;
    if (view == "NFD"_s)
        return Unicode::NormalizationForm::NFD;
    if (view == "NFKD"_s)
        return Unicode::NormalizationForm::NFKD;
    raiseValueError(globalObject, scope, "invalid normalization form"_s);
    return std::nullopt;
}

} // anonymous namespace

#define UCD_PROLOGUE() \
    auto [previous, first] = callOf(callFrame); \
    NATIVE_PROLOGUE(); \
    UNUSED_VARIABLE(previous)

#define CHARACTER_ARGUMENT(function, argument) \
    auto character = toCharacterArgument(globalObject, args[first], function, argument); \
    RETURN_IF_EXCEPTION(scope, { }); \
    char32_t c = *character

// decimal(chr, default=<unrepresentable>, /)
PYTHON_NATIVE(ucdDecimal)
{
    UCD_PROLOGUE();
    CHARACTER_ARGUMENT("decimal"_s, "argument 1"_s);
    std::optional<int> old;
    if (previous) {
        auto* record = previous->record(c);
        if (!record->categoryChanged)
            old = -1; // It was not assigned.
        else if (record->decimalChanged != 0xFF)
            old = record->decimalChanged;
    }
    int result = old ? *old : Unicode::toDecimalDigit(c);
    if (result >= 0)
        return JSValue::encode(jsNumber(result));
    if (JSValue defaultValue = args.at(first + 1))
        return JSValue::encode(defaultValue);
    return JSValue::encode(raiseValueError(globalObject, scope, "not a decimal"_s));
}

// digit(chr, default=<unrepresentable>, /)
PYTHON_NATIVE(ucdDigit)
{
    UCD_PROLOGUE();
    CHARACTER_ARGUMENT("digit"_s, "argument 1"_s);
    int result = Unicode::toDigit(c);
    if (result >= 0)
        return JSValue::encode(jsNumber(result));
    if (JSValue defaultValue = args.at(first + 1))
        return JSValue::encode(defaultValue);
    return JSValue::encode(raiseValueError(globalObject, scope, "not a digit"_s));
}

// numeric(chr, default=<unrepresentable>, /)
PYTHON_NATIVE(ucdNumeric)
{
    UCD_PROLOGUE();
    CHARACTER_ARGUMENT("numeric"_s, "argument 1"_s);
    std::optional<double> old;
    if (previous) {
        auto* record = previous->record(c);
        if (!record->categoryChanged)
            old = -1.0;
        else if (record->numericChanged)
            old = record->numericChanged;
    }
    double result = old ? *old : Unicode::toNumeric(c);
    if (result != -1.0)
        return JSValue::encode(floatFromDouble(result));
    if (JSValue defaultValue = args.at(first + 1))
        return JSValue::encode(defaultValue);
    return JSValue::encode(raiseValueError(globalObject, scope, "not a numeric character"_s));
}

// category(chr, /)
PYTHON_NATIVE(ucdCategory)
{
    UCD_PROLOGUE();
    CHARACTER_ARGUMENT("category"_s, "argument"_s);
    unsigned index = recordOf(c).category;
    if (previous && previous->record(c)->categoryChanged != 0xFF)
        index = previous->record(c)->categoryChanged;
    return JSValue::encode(jsString(vm, String(categoryNames[index])));
}

// bidirectional(chr, /)
PYTHON_NATIVE(ucdBidirectional)
{
    UCD_PROLOGUE();
    CHARACTER_ARGUMENT("bidirectional"_s, "argument"_s);
    unsigned index = recordOf(c).bidirectional;
    if (previous) {
        auto* record = previous->record(c);
        if (!record->categoryChanged)
            index = 0;
        else if (record->bidirectionalChanged != 0xFF)
            index = record->bidirectionalChanged;
    }
    return JSValue::encode(jsString(vm, String(bidirectionalNames[index])));
}

// combining(chr, /)
PYTHON_NATIVE(ucdCombining)
{
    UCD_PROLOGUE();
    CHARACTER_ARGUMENT("combining"_s, "argument"_s);
    if (previous && !previous->record(c)->categoryChanged)
        return JSValue::encode(jsNumber(0));
    return JSValue::encode(jsNumber(recordOf(c).combining));
}

// mirrored(chr, /)
PYTHON_NATIVE(ucdMirrored)
{
    UCD_PROLOGUE();
    CHARACTER_ARGUMENT("mirrored"_s, "argument"_s);
    unsigned index = recordOf(c).mirrored;
    if (previous) {
        auto* record = previous->record(c);
        if (!record->categoryChanged)
            index = 0;
        else if (record->mirroredChanged != 0xFF)
            index = record->mirroredChanged;
    }
    return JSValue::encode(jsNumber(index));
}

// east_asian_width(chr, /)
PYTHON_NATIVE(ucdEastAsianWidth)
{
    UCD_PROLOGUE();
    CHARACTER_ARGUMENT("east_asian_width"_s, "argument"_s);
    unsigned index = recordOf(c).eastAsianWidth;
    if (previous) {
        auto* record = previous->record(c);
        if (!record->categoryChanged)
            index = 0;
        else if (record->eastAsianWidthChanged != 0xFF)
            index = record->eastAsianWidthChanged;
    }
    return JSValue::encode(jsString(vm, String(eastAsianWidthNames[index])));
}

// decomposition(chr, /)
PYTHON_NATIVE(ucdDecomposition)
{
    UCD_PROLOGUE();
    CHARACTER_ARGUMENT("decomposition"_s, "argument"_s);
    if (previous && !previous->record(c)->categoryChanged)
        return JSValue::encode(jsEmptyString(vm));
    TextBuilder builder;
    auto append = [&] (char32_t code) {
        if (!builder.isEmpty())
            builder.append(' ');
        builder.append(hex(static_cast<unsigned>(code), 4));
    };
    if (isHangulSyllable(c)) {
        unsigned sIndex = c - sBase;
        append(lBase + sIndex / nCount);
        append(vBase + (sIndex % nCount) / tCount);
        if (sIndex % tCount)
            append(tBase + sIndex % tCount);
    } else {
        // Not what it was in an earlier version, all but that it was not assigned
        auto decomposition = decompositionOf(nullptr, c);
        builder.append(decompositionPrefixes[decomposition.prefix]);
        for (unsigned i = 0; i < decomposition.count; ++i)
            append(decompositionData[decomposition.index + i]);
    }
    return JSValue::encode(jsString(vm, builder.toString()));
}

// name(chr, default=<unrepresentable>, /)
PYTHON_NATIVE(ucdName)
{
    UCD_PROLOGUE();
    CHARACTER_ARGUMENT("name"_s, "argument 1"_s);
    NameBuffer buffer;
    if (nameOf(previous, c, false, buffer))
        return JSValue::encode(jsString(vm, String(byteCast<Latin1Character>(buffer.span()))));
    if (JSValue defaultValue = args.at(first + 1))
        return JSValue::encode(defaultValue);
    return JSValue::encode(raiseValueError(globalObject, scope, "no such name"_s));
}

// lookup(name, /)
PYTHON_NATIVE(ucdLookup)
{
    UCD_PROLOGUE();
    // "s#": a str as UTF-8, or the bytes of what cannot be written to
    JSValue given = args[first];
    Vector<char, 64> name;
    if (stringIn(given)) {
        auto encoded = encodeUTF8(globalObject, given, "strict"_s);
        RETURN_IF_EXCEPTION(scope, { });
        name.append(byteCast<char>(encoded->span()));
    } else {
        // convertbuffer(): not of what has to be told when its bytes are done with, since it never would be. That is all that is built in but bytes.
        bool hasToBeReleased = isInstance(globalObject, given, realm->typeBytes()) ? false : builtinBufferOf(given) || typeOf(globalObject, given)->lookup(vm, names.dunder_release_buffer);
        if (hasToBeReleased)
            return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("lookup() argument must be read-only bytes-like object, not "_s, typeNameOfArgument(globalObject, given))));
        Buffer buffer = bufferOf(globalObject, given);
        RETURN_IF_EXCEPTION(scope, { });
        name.append(byteCast<char>(buffer.span()));
    }
    size_t length = name.size();
    name.append('\0');
    if (length > nameMaxLength)
        return JSValue::encode(raise(globalObject, scope, BuiltinType::KeyError, "name too long"_s));
    auto undefined = [&] {
        // "%s": as far as the first zero, and taken for UTF-8
        return JSValue::encode(raise(globalObject, scope, BuiltinType::KeyError, concatenate("undefined character name '"_s, String::fromUTF8ReplacingInvalidSequences(byteCast<char8_t>(unsafeSpan(name.span().data()))), '\'')));
    };
    auto code = codeNamed(name.span().first(length));
    if (!code)
        return undefined();
    if (previous && (isAlias(*code) || isNamedSequence(*code)))
        return undefined();
    if (isNamedSequence(*code)) {
        auto& sequence = namedSequences[*code - namedSequencesStart];
        return JSValue::encode(jsString(vm, String(std::span(sequence.characters).first(static_cast<size_t>(sequence.length)))));
    }
    char32_t result = isAlias(*code) ? nameAliases[*code - aliasesStart] : *code;
    RELEASE_AND_RETURN(scope, JSValue::encode(strOfCharacters(globalObject, std::span(&result, 1))));
}

// `unicode` of Argument Clinic, twice over. False if it raised.
static bool toFormAndText(JSGlobalObject* globalObject, const NativeArguments& args, unsigned first, ASCIILiteral function, JSString*& form, JSString*& text)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    form = stringIn(args[first]);
    if (!form) {
        raiseTypeError(globalObject, scope, concatenate(function, "() argument 1 must be str, not "_s, typeNameOfArgument(globalObject, args[first])));
        return false;
    }
    text = stringIn(args[first + 1]);
    if (!text) {
        raiseTypeError(globalObject, scope, concatenate(function, "() argument 2 must be str, not "_s, typeNameOfArgument(globalObject, args[first + 1])));
        return false;
    }
    return true;
}

// is_normalized(form, unistr, /)
PYTHON_NATIVE(ucdIsNormalized)
{
    UCD_PROLOGUE();
    JSString* formString;
    JSString* text;
    if (!toFormAndText(globalObject, args, first, "is_normalized"_s, formString, text))
        return { };
    // Nothing at all is in any form, whatever it is called.
    if (!text->length())
        return JSValue::encode(jsBoolean(true));
    auto form = toForm(globalObject, formString);
    RETURN_IF_EXCEPTION(scope, { });
    auto view = text->view(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    Vector<char32_t, 64> characters;
    if (!charactersOf(globalObject, view, characters))
        return { };
    auto checked = quickCheck(previous, characters.span(), *form, false);
    if (checked != QuickCheck::Maybe)
        return JSValue::encode(jsBoolean(checked == QuickCheck::Yes));
    Vector<char32_t> normalized;
    if (!tryNormalize(previous, *form, characters.span(), normalized))
        return JSValue::encode(raiseMemoryError(globalObject, scope));
    return JSValue::encode(jsBoolean(std::ranges::equal(characters, normalized)));
}

// normalize(form, unistr, /)
PYTHON_NATIVE(ucdNormalize)
{
    UCD_PROLOGUE();
    JSString* formString;
    JSString* text;
    if (!toFormAndText(globalObject, args, first, "normalize"_s, formString, text))
        return { };
    if (!text->length())
        return JSValue::encode(text);
    auto form = toForm(globalObject, formString);
    RETURN_IF_EXCEPTION(scope, { });
    auto view = text->view(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    Vector<char32_t, 64> characters;
    if (!charactersOf(globalObject, view, characters))
        return { };
    if (quickCheck(previous, characters.span(), *form, true) == QuickCheck::Yes)
        return JSValue::encode(text);
    Vector<char32_t> normalized;
    if (!tryNormalize(previous, *form, characters.span(), normalized))
        return JSValue::encode(raiseMemoryError(globalObject, scope));
    RELEASE_AND_RETURN(scope, JSValue::encode(strOfCharacters(globalObject, normalized.span())));
}

JSObject* createUnicodeDataModule(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    PyRealm* realm = globalObject->pyRealm();
    auto& state = realm->moduleState<UnicodeDataModuleState>();
    struct Function {
        ASCIILiteral name;
        NativeFunction function;
    };
    static constexpr Function functions[] = {
        { "decimal"_s, ucdDecimal },
        { "digit"_s, ucdDigit },
        { "numeric"_s, ucdNumeric },
        { "category"_s, ucdCategory },
        { "bidirectional"_s, ucdBidirectional },
        { "combining"_s, ucdCombining },
        { "mirrored"_s, ucdMirrored },
        { "east_asian_width"_s, ucdEastAsianWidth },
        { "decomposition"_s, ucdDecomposition },
        { "name"_s, ucdName },
        { "lookup"_s, ucdLookup },
        { "is_normalized"_s, ucdIsNormalized },
        { "normalize"_s, ucdNormalize },
    };
    if (!state.ucd) {
        PyType* type = createBuiltinType(globalObject, "unicodedata.UCD"_s, realm->typeObject(), PyType::Layout::Native, 0);
        type->setInstanceStructure(vm, PyStateObject::createStructure(vm, globalObject, type));
        state.ucd.set(vm, realm, type);
        addGenericGetAttribute(globalObject, type);
        for (auto& function : functions)
            addMethods(globalObject, type, { { function.name, function.function, PyNativeFunction::Kind::Method, pack(true) } });
        addMember(globalObject, type, "unidata_version"_s, [] (JSGlobalObject* globalObject, JSValue self) -> JSValue { return jsString(globalObject->vm(), String(stateOf<PreviousVersionState>(self).version.name)); });
        // The version that the IDNA encoding goes by
        state.ucd_3_2_0.set(vm, realm, PyStateObject::create(vm, type->instanceStructure(), makeUnique<PreviousVersionState>(version_3_2_0)));
    }
    JSObject* module = newBuiltinModule(globalObject, "unicodedata"_s);
    for (auto& function : functions)
        addFunction(globalObject, module, function.name, function.function, pack(false));
    module->putDirect(vm, Identifier::fromString(vm, "unidata_version"_s), jsString(vm, String(unidataVersion)));
    module->putDirect(vm, Identifier::fromString(vm, "UCD"_s), state.ucd.get());
    module->putDirect(vm, Identifier::fromString(vm, "ucd_3_2_0"_s), state.ucd_3_2_0.get());
    module->putDirect(vm, Identifier::fromString(vm, "_ucnhash_CAPI"_s), newCapsule(globalObject, "unicodedata._ucnhash_CAPI"_s));
    return module;
}

} } // namespace JSC::Python
