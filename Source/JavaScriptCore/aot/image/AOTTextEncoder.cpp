/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTextEncoder.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTIRProgram.h"
#include "AOTIdentifierNumbering.h"
#include "AOTSectionWriter.h"
#include "AOTThreads.h"
#include "AOTVector.h"
#include <algorithm>
#include <bit>
#include <compare>
#include <cstring>
#include <functional>
#include <limits>
#include <optional>
#include <wtf/ASCIICType.h>
#include <wtf/BitVector.h>
#include <wtf/HashMap.h>
#include <wtf/StdLibExtras.h>
#include <wtf/text/ASCIIFastPath.h>
#include <wtf/text/StringHasherInlines.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

static constexpr size_t headSize = sizeof(uint32_t);
static constexpr unsigned headSlotBits = 22;

namespace {

struct StringBytes {
    std::span<const uint8_t> operator[](uint32_t index) const { return strings[index].bytes(); }

    std::span<const DecodedString> strings;
};

struct OrderedString {
    const uint8_t* characters;
    uint32_t size;
    uint32_t index;
    uint64_t afterHead;
};

struct HeadIndex {
    Vector<OrderedString> orderedStrings;
    Vector<std::optional<uint32_t>> longestHeads;
    UncheckedKeyHashMap<uint64_t, std::pair<uint32_t, uint32_t>> headRanges;
    BitVector mayBeHead;
    Vector<uint32_t> fromLongest;
};

} // anonymous namespace

static void sortByPrefixThenBy(const ParallelLoops& loops, Vector<uint32_t>& list, const auto& prefixOf, const auto& isLess)
{
    Vector<std::pair<uint64_t, uint32_t>> withBytes(list.size(), [&](size_t i) {
        return std::pair { prefixOf(list[i]), list[i] };
    });
    parallelSort(loops, withBytes.mutableSpan(), [&](const auto& a, const auto& b) {
        return a.first != b.first ? a.first < b.first : isLess(a.second, b.second);
    });
    for (size_t i = 0; i < list.size(); ++i)
        list[i] = withBytes[i].second;
}

static uint64_t prefixFrom(auto at, auto end)
{
    uint64_t result = 0;
    for (unsigned shift = 64; shift && at != end; ++at)
        result |= static_cast<uint64_t>(*at) << (shift -= 8);
    return result;
}

static uint64_t headAt(std::span<const uint8_t> text, size_t at)
{
    uint32_t head;
    memcpy(&head, text.data() + at, headSize);
    return static_cast<uint64_t>(head) + 1;
}

static uint64_t prefixAfterHead(std::span<const uint8_t> text)
{
    if (text.size() >= headSize + sizeof(uint64_t)) {
        uint64_t bytes;
        memcpy(&bytes, text.data() + headSize, sizeof(bytes));
        static_assert(std::endian::native == std::endian::little);
        return std::byteswap(bytes);
    }
    uint64_t result = 0;
    size_t end = std::min(text.size(), headSize + sizeof(uint64_t));
    for (size_t at = headSize; at < end; ++at)
        result |= static_cast<uint64_t>(text[at]) << (8 * (headSize + sizeof(uint64_t) - 1 - at));
    return result;
}

static size_t headSlot(uint64_t head)
{
    return static_cast<size_t>(head * 0x9e3779b97f4a7c15ULL >> (64 - headSlotBits));
}

static void shareTails(const ParallelLoops& loops, StringBytes strings, const BitVector& hasText, const BitVector& isName, Vector<uint32_t>& holders)
{
    Vector<uint32_t> byLastCharacters;
    for (size_t index : hasText)
        byLastCharacters.append(static_cast<uint32_t>(index));
    sortByPrefixThenBy(loops, byLastCharacters, [&](uint32_t index) {
        return prefixFrom(strings[index].rbegin(), strings[index].rend());
    }, [&](uint32_t a, uint32_t b) {
        auto [endOfA, endOfB] = std::mismatch(strings[a].rbegin(), strings[a].rend(), strings[b].rbegin(), strings[b].rend());
        if (endOfA == strings[a].rend() || endOfB == strings[b].rend())
            return strings[a].size() != strings[b].size() ? strings[a].size() < strings[b].size() : a < b;
        return *endOfA < *endOfB;
    });
    for (size_t i = byLastCharacters.size(); i-- > 1;) {
        auto tail = strings[byLastCharacters[i - 1]];
        auto whole = strings[byLastCharacters[i]];
        if (tail.size() <= whole.size() && equalSpans(tail, whole.last(tail.size())) && (!isName.get(byLastCharacters[i - 1]) || isName.get(holders[byLastCharacters[i]])))
            holders[byLastCharacters[i - 1]] = holders[byLastCharacters[i]];
    }
}

static HeadIndex buildHeadIndex(const ParallelLoops& loops, StringBytes strings, const BitVector& hasText, const Vector<uint32_t>& holders)
{
    Vector<uint32_t> inOrder;
    for (size_t bit : hasText) {
        uint32_t index = static_cast<uint32_t>(bit);
        if (holders[index] == index && strings[index].size() >= headSize)
            inOrder.append(index);
    }
    sortByPrefixThenBy(loops, inOrder, [&](uint32_t index) {
        return prefixFrom(strings[index].begin(), strings[index].end());
    }, [&](uint32_t a, uint32_t b) {
        auto order = std::lexicographical_compare_three_way(strings[a].begin(), strings[a].end(), strings[b].begin(), strings[b].end());
        return order != 0 ? order < 0 : a < b;
    });

    HeadIndex result;
    result.orderedStrings = Vector<OrderedString>(inOrder.size(), [&](size_t position) {
        auto text = strings[inOrder[position]];
        return OrderedString { text.data(), safeCast<uint32_t>(text.size()), inOrder[position], prefixAfterHead(text) };
    });
    result.longestHeads = Vector<std::optional<uint32_t>>(FillWith { }, inOrder.size(), std::nullopt);
    result.mayBeHead.ensureSize(1 << headSlotBits);
    Vector<uint32_t> open;
    for (uint32_t position = 0; position < inOrder.size(); ++position) {
        auto text = strings[inOrder[position]];
        while (!open.isEmpty() && !spanHasPrefix(text, strings[inOrder[open.last()]]))
            open.removeLast();
        if (!open.isEmpty())
            result.longestHeads[position] = open.last();
        open.append(position);
        result.headRanges.add(headAt(text, 0), std::pair { position, position }).iterator->value.second = position + 1;
        result.mayBeHead.quickSet(headSlot(headAt(text, 0)));
    }
    Vector<uint64_t> lengthsAndNumbers(inOrder.size(), [&](size_t position) {
        return static_cast<uint64_t>(std::numeric_limits<uint32_t>::max() - safeCast<uint32_t>(strings[inOrder[position]].size())) << 32 | inOrder[position];
    });
    parallelSort(loops, lengthsAndNumbers.mutableSpan(), std::less<uint64_t> { });
    result.fromLongest = Vector<uint32_t>(lengthsAndNumbers.size(), [&](size_t position) {
        return static_cast<uint32_t>(lengthsAndNumbers[position]);
    });
    return result;
}

static bool ordersBefore(std::span<const uint8_t> a, uint64_t afterHeadOfA, const OrderedString& b, uint64_t& charactersCompared)
{
    constexpr size_t recordedSize = headSize + sizeof(uint64_t);
    size_t shorterSize = std::min<size_t>(a.size(), b.size);
    uint64_t difference = afterHeadOfA ^ b.afterHead;
    size_t same = headSize + (difference ? std::countl_zero(difference) / 8 : sizeof(uint64_t));
    if (same < std::min(shorterSize, recordedSize)) {
        charactersCompared += same;
        return afterHeadOfA < b.afterHead;
    }
    if (shorterSize <= recordedSize) {
        charactersCompared += shorterSize;
        return b.size > shorterSize;
    }
    auto restOfA = a.subspan(recordedSize);
    std::span restOfB { b.characters + recordedSize, b.size - recordedSize };
    auto [endOfA, endOfB] = std::ranges::mismatch(restOfA, restOfB);
    charactersCompared += recordedSize + (endOfA - restOfA.begin());
    return endOfB != restOfB.end() && (endOfA == restOfA.end() || *endOfA < *endOfB);
}

static std::optional<uint32_t> findLastNotAfter(const HeadIndex& heads, std::span<const uint8_t> rest, uint64_t& charactersCompared)
{
    uint64_t head = headAt(rest, 0);
    if (!heads.mayBeHead.quickGet(headSlot(head)))
        return std::nullopt;
    auto found = heads.headRanges.find(head);
    if (found == heads.headRanges.end())
        return std::nullopt;
    uint64_t afterHeadOfRest = prefixAfterHead(rest);
    auto [first, afterLast] = found->value;
    uint32_t low = first;
    for (uint32_t high = afterLast; low < high;) {
        uint32_t middle = low + (high - low) / 2;
        if (ordersBefore(rest, afterHeadOfRest, heads.orderedStrings[middle], charactersCompared))
            high = middle;
        else
            low = middle + 1;
    }
    if (low == first)
        return std::nullopt;
    return low - 1;
}

static void findInnerParts(StringBytes strings, const BitVector& isName, const HeadIndex& heads, size_t unitSize, uint64_t maxComparedCharactersPerCharacter, TextSharing& sharing)
{
    auto& holders = sharing.holders;
    auto& gaps = sharing.gaps;
    uint64_t charactersCompared = 0;
    for (uint32_t whole : heads.fromLongest) {
        if (holders[whole] != whole)
            continue;
        auto text = strings[whole];
        uint64_t maxCharactersCompared = charactersCompared + maxComparedCharactersPerCharacter * static_cast<uint64_t>(text.size());
        for (size_t at = 0; at + headSize <= text.size(); at += unitSize) {
            if (charactersCompared > maxCharactersCompared) {
                ++sharing.searchLimitHits;
                break;
            }
            auto rest = text.subspan(at);
            for (auto position = findLastNotAfter(heads, rest, charactersCompared); position; position = heads.longestHeads[*position]) {
                const OrderedString& candidate = heads.orderedStrings[*position];
                uint32_t part = candidate.index;
                std::span inner { candidate.characters, candidate.size };
                if (inner.size() >= text.size())
                    continue;
                charactersCompared += inner.size();
                if (!spanHasPrefix(rest, inner))
                    continue;
                if (isName.get(part) && !isName.get(whole))
                    continue;
                if (holders[part] != part)
                    break;
                holders[part] = whole;
                gaps[part] = safeCast<uint32_t>(rest.size() - inner.size());
            }
        }
    }
}

TextSharing shareText(std::span<const DecodedString> decodedStrings, const BitVector& hasText, const BitVector& isName, BuildContext& context)
{
    const ParallelLoops& loops = context.parallelLoops();
    StringBytes strings { decodedStrings };
    TextSharing sharing;
    auto& holders = sharing.holders;
    holders = Vector<uint32_t>(decodedStrings.size(), [](size_t index) {
        return static_cast<uint32_t>(index);
    });
    sharing.gaps.fill(0, decodedStrings.size());

    for (size_t unitSize : { sizeof(uint8_t), sizeof(char16_t) }) {
        BitVector hasTextOfWidth(decodedStrings.size());
        for (size_t index : hasText) {
            if (decodedStrings[index].is16Bit() == (unitSize == sizeof(char16_t)))
                hasTextOfWidth.quickSet(index);
        }
        if (hasTextOfWidth.isEmpty())
            continue;
        shareTails(loops, strings, hasTextOfWidth, isName, holders);
        findInnerParts(strings, isName, buildHeadIndex(loops, strings, hasTextOfWidth, holders), unitSize, context.limit(LimitID::MaxComparedCharactersPerCharacter), sharing);
        for (size_t index : hasTextOfWidth) {
            uint32_t holder = holders[index];
            if (holders[holder] == holder)
                continue;
            sharing.gaps[index] = sharing.gaps[holder];
            holders[index] = holders[holder];
        }
    }
    return sharing;
}

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

static_assert(ImageString::hashMask == StringHasher::maskHash && ImageIdentifier::hashMask == StringHasher::maskHash);
static_assert(defaultLimitValue(LimitID::MaxIdentifiers, hostArchitecture()) == ImageString::hashMask + 1);
static_assert(defaultLimitValue(LimitID::MaxListedStrings, hostArchitecture()) == 1u << (32 - ImageString::lengthBits));
static_assert(numberOfEnumerators<PropertyKeyKind> && numberOfEnumerators<PropertyKeyKind> <= 1u << ImageIdentifier::kindBits);
static_assert(numberOfEnumerators<NameClass> && numberOfEnumerators<NameClass> <= 1u << ImageIdentifier::nameClassBits);
static_assert(numberOfEnumerators<Typed::WellKnownSymbol> && numberOfEnumerators<Typed::WellKnownSymbol> <= ImageIdentifier::hashMask + 1);
static_assert(sizeof(DecodedString) >= sizeof(ImageString));

namespace {

struct RetainedStrings {
    BitVector isName;
    BitVector isRetained;
    Vector<DecodedString> decoded;
    TextSharing sharing;
};

} // anonymous namespace

DecodedString decodeForImage(std::span<const uint8_t> programString)
{
    DecodedString result;
    if (charactersAreAllASCII(byteCast<Latin1Character>(programString)))
        result.units8 = Vector<uint8_t>(programString);
    else {
        Vector<char16_t> units = Typed::decodeProgramString(programString);
        if (WTF::charactersAreAllLatin1(units.span())) {
            result.units8 = Vector<uint8_t>(units.size(), [&](size_t index) {
                return static_cast<uint8_t>(units[index]);
            });
        } else
            result.units16 = WTF::move(units);
    }
    result.hash = result.is16Bit()
        ? StringHasher::computeHashAndMaskTop8Bits(result.units16.span())
        : StringHasher::computeHashAndMaskTop8Bits(byteCast<Latin1Character>(result.units8.span()));
    return result;
}

static Vector<uint32_t> appendText(Vector<uint8_t>& text, const TextInputs& inputs, const RetainedStrings& strings)
{
    const Typed::Program& program = inputs.program;
    const StringUses& uses = inputs.uses;
    text.append(0);
    Vector<uint32_t> ends(FillWith { }, program.strings.size(), 0);
    BitVector isEmitted(program.strings.size());
    auto emitHolderOf = [&](size_t index) {
        if (!strings.isRetained.quickGet(index))
            return;
        uint32_t holder = strings.sharing.holders[index];
        if (isEmitted.quickSet(holder))
            return;
        const DecodedString& string = strings.decoded[holder];
        std::span<const uint8_t> bytes = string.bytes();
        if (string.is16Bit() && text.size() % sizeof(char16_t))
            text.append(0);
        text.append(bytes);
        ends[holder] = static_cast<uint32_t>(text.size());
    };
    for (uint32_t functionIndex : uses.functionsInSourceOrder) {
        for (uint32_t named : uses.namedByFunction[functionIndex]) {
            if (strings.isName.quickGet(named))
                emitHolderOf(named);
        }
    }
    for (size_t index : strings.isName)
        emitHolderOf(index);
    for (uint32_t functionIndex : uses.functionsInSourceOrder) {
        if (StringID name = program.functions[functionIndex].name)
            emitHolderOf(name.index());
        for (uint32_t named : uses.namedByFunction[functionIndex])
            emitHolderOf(named);
    }
    for (size_t index : strings.isRetained)
        emitHolderOf(index);
    return ends;
}

[[nodiscard]] static bool encodeStringRecords(EncodedText& result, const TextInputs& inputs, const RetainedStrings& strings, const Vector<uint32_t>& ends, BuildContext& context)
{
    const StringUses& uses = inputs.uses;
    Vector<ImageString> records(FillWith { }, strings.decoded.size(), ImageString { 0, 0 });
    Vector<ImageLongString> longStrings { ImageLongString { 0, 0 } };
    for (size_t index : strings.isRetained) {
        const DecodedString& string = strings.decoded[index];
        uint32_t end = ends[strings.sharing.holders[index]];
        uint64_t distanceToEnd = static_cast<uint64_t>(strings.sharing.gaps[index]) + string.bytes().size();
        RELEASE_ASSERT(end > distanceToEnd);
        uint32_t offset = end - static_cast<uint32_t>(distanceToEnd);
        RELEASE_ASSERT(!string.is16Bit() || !(offset % sizeof(char16_t)));

        bool isAtom = uses.isAtom.get(index) || strings.isName.quickGet(index);
        bool isNamedByTables = inputs.stringsNamedByTables.get(index);
        bool isOnlyShownByTables = inputs.stringsOnlyShownByTables && inputs.stringsOnlyShownByTables->get(index);
        bool isReadThroughHeader = isAtom || uses.isNamedBesideCreation.get(index) || (isNamedByTables && !isOnlyShownByTables);
        bool hasHeader = isReadThroughHeader || (!uses.isNamed.get(index) && !isNamedByTables);
        uint32_t hashAndFlags = string.hash;
        if (std::optional<uint32_t> identifierNumber = inputs.identifiers.numberOfText(StringID(index))) {
            RELEASE_ASSERT(*identifierNumber <= ImageString::hashMask);
            hashAndFlags = ImageString::hasIdentifierNumberFlag | *identifierNumber;
        }
        if (!hasHeader)
            hashAndFlags |= ImageString::hasNoHeaderFlag;
        // Whoever makes the cell of the constant makes the header: see ProgramStrings::tryCreateHeader().
        if (uses.isConstant.get(index) && !isReadThroughHeader)
            hashAndFlags |= ImageString::hasListedHashFlag;
        if (string.is16Bit())
            hashAndFlags |= ImageString::is16BitFlag;
        if (isAtom)
            hashAndFlags |= ImageString::isAtomFlag;

        uint32_t location = offset << ImageString::lengthBits | string.length();
        if (!hasHeader || !ImageListedString::fitsOneWord({ offset, string.length() })) {
            location = static_cast<uint32_t>(longStrings.size()) << ImageString::lengthBits;
            longStrings.append(ImageLongString { offset, string.length() });
        }
        records[index] = ImageString { location, hashAndFlags };
    }
    if (longStrings.size() > context.limit(LimitID::MaxListedStrings)) {
        context.reportLimitExceeded(LimitID::MaxListedStrings, longStrings.size());
        return false;
    }
    result.strings = bytesOf(records);
    result.longStrings = bytesOf(longStrings);
    return true;
}

static uint32_t hashOrSymbolOf(const Typed::PropertyKey& key, uint32_t textHash)
{
    switch (key.kind) {
    case PropertyKeyKind::WellKnownSymbol:
        RELEASE_ASSERT(key.symbol);
        return static_cast<uint32_t>(*key.symbol);
    case PropertyKeyKind::Text:
    case PropertyKeyKind::EnginePrivateName:
    case PropertyKeyKind::PrivateName:
    case PropertyKeyKind::CreatedSymbol:
    case PropertyKeyKind::RegisteredSymbol:
        return textHash;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ImageIdentifier encodeIdentifier(const Typed::Program& program, const NumberedIdentifier& identifier, std::span<const DecodedString> decoded)
{
    uint32_t textHash = identifier.text ? decoded[identifier.text.index()].hash : 0;
    if (!identifier.key) {
        RELEASE_ASSERT(identifier.text);
        return ImageIdentifier::create(identifier.text, textHash, PropertyKeyKind::Text, program.nameClass(identifier.text));
    }
    const Typed::PropertyKey& key = program.at(identifier.key);
    return ImageIdentifier::create(identifier.text, hashOrSymbolOf(key, textHash), key.kind, program.nameClassOf(identifier.key));
}

EncodedText encodeText(const TextInputs& inputs, BuildContext& context)
{
    if (!checkSectionSize(static_cast<uint64_t>(inputs.program.strings.size()) * sizeof(DecodedString), context))
        return { };

    const Typed::StringTable& programStrings = inputs.program.strings;
    RetainedStrings strings;
    strings.isName.ensureSize(programStrings.size());
    for (uint32_t number = 1; number < inputs.identifiers.count(); ++number) {
        if (StringID text = inputs.identifiers.at(number).text)
            strings.isName.quickSet(text.index());
    }
    strings.isRetained.ensureSize(programStrings.size());
    strings.isRetained.merge(inputs.stringsNamedByTables);
    strings.isRetained.merge(inputs.uses.isNamed);
    strings.isRetained.merge(strings.isName);
    strings.isRetained.quickSet(programStrings.emptyString().index());
    strings.decoded.grow(programStrings.size());
    forEachIndexInParallel(context.parallelLoops(), static_cast<unsigned>(programStrings.size()), [&](unsigned index) {
        if (strings.isRetained.quickGet(index))
            strings.decoded[index] = decodeForImage(programStrings[StringID(index)]);
    });
    strings.sharing = shareText(strings.decoded.span(), strings.isRetained, strings.isName, context);
    context.statistics().count(hitsOf(LimitID::MaxComparedCharactersPerCharacter), strings.sharing.searchLimitHits);

    uint64_t maxTextSize = 1;
    for (size_t index : strings.isRetained) {
        if (strings.sharing.holders[index] == index)
            maxTextSize += strings.decoded[index].bytes().size() + 1;
    }
    if (!checkSectionSize(maxTextSize, context))
        return { };

    EncodedText result;
    result.text.reserveInitialCapacity(static_cast<size_t>(maxTextSize));
    Vector<uint32_t> ends = appendText(result.text, inputs, strings);
    if (!encodeStringRecords(result, inputs, strings, ends, context))
        return { };
    result.identifiers = bytesOf(Vector<ImageIdentifier>(inputs.identifiers.count(), [&](size_t number) {
        if (!number)
            return ImageIdentifier { 0, 0 };
        return encodeIdentifier(inputs.program, inputs.identifiers.at(static_cast<uint32_t>(number)), strings.decoded.span());
    }));
    result.stringConstants = bytesOf(Vector<uint32_t>(inputs.constants.count(), [&](size_t number) {
        return inputs.constants.stringOf(static_cast<uint32_t>(number)).index();
    }));
    context.destroyLater(WTF::move(strings));
    return result;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
