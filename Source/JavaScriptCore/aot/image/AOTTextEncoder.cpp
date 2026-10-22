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
#include <optional>
#include <wtf/ASCIICType.h>
#include <wtf/StdLibExtras.h>
#include <wtf/text/ASCIIFastPath.h>
#include <wtf/text/StringHasherInlines.h>

namespace JSC { namespace AOT {

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

static BitVector findNames(const TextInputs& inputs)
{
    BitVector isName(inputs.program.strings.size());
    for (uint32_t number = 1; number < inputs.identifiers.count(); ++number) {
        if (StringID text = inputs.identifiers.at(number).text)
            isName.quickSet(text.index());
    }
    return isName;
}

static BitVector findRetainedStrings(const TextInputs& inputs, const BitVector& isName)
{
    BitVector isRetained(inputs.program.strings.size());
    isRetained.merge(inputs.stringsNamedByTables);
    isRetained.merge(inputs.uses.isNamed);
    isRetained.merge(isName);
    for (StringID string : inputs.constants.strings())
        isRetained.quickSet(string.index());
    isRetained.quickSet(inputs.program.strings.emptyString().index());
    return isRetained;
}

static Vector<DecodedString> decodeRetainedStrings(const Typed::StringTable& strings, const BitVector& isRetained, BuildContext& context)
{
    Vector<DecodedString> decoded(strings.size());
    forEachIndexInParallel(context.parallelLoops(), static_cast<unsigned>(strings.size()), [&](unsigned index) {
        if (isRetained.quickGet(index))
            decoded[index] = decodeForImage(strings[StringID(index)]);
    });
    return decoded;
}

// Counts a byte before every holder. appendText() adds one only to align 16-bit text.
static uint64_t computeMaxTextSize(const RetainedStrings& strings)
{
    uint64_t maxSize = 1;
    for (size_t index : strings.isRetained) {
        if (strings.sharing.holders[index] == index)
            maxSize += strings.decoded[index].bytes().size() + 1;
    }
    return maxSize;
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

// In the image nothing but the list says where a string without a header is.
static uint32_t encodeLocation(uint32_t offset, uint32_t length, bool hasHeader, Vector<ImageLongString>& longStrings)
{
    if (hasHeader && length && length <= ImageString::lengthMask && !(offset >> (32 - ImageString::lengthBits)))
        return offset << ImageString::lengthBits | length;
    uint32_t longStringIndex = static_cast<uint32_t>(longStrings.size());
    longStrings.append(ImageLongString { offset, length });
    return longStringIndex << ImageString::lengthBits;
}

// By a stub that compares with the text, by the atom table, by whoever takes the string out of a table as a value.
static bool isReadThroughHeader(const TextInputs& inputs, const RetainedStrings& strings, size_t index)
{
    const StringUses& uses = inputs.uses;
    if (strings.isName.quickGet(index) || uses.isAtom.get(index) || uses.isNamedBesideCreation.get(index))
        return true;
    return inputs.stringsNamedByTables.get(index) && !(inputs.stringsOnlyShownByTables && inputs.stringsOnlyShownByTables->get(index));
}

// Whoever makes the cell of the constant makes the header: see ProgramStrings::tryCreateHeader().
static bool getsHeaderWithCell(const TextInputs& inputs, const RetainedStrings& strings, size_t index)
{
    return inputs.uses.isConstant.get(index) && !isReadThroughHeader(inputs, strings, index);
}

// A string that only a message shows becomes a String when the message is made.
static bool hasHeader(const TextInputs& inputs, const RetainedStrings& strings, size_t index)
{
    if (isReadThroughHeader(inputs, strings, index))
        return true;
    return !inputs.uses.isNamed.get(index) && !inputs.stringsNamedByTables.get(index);
}

static uint32_t encodeHashAndFlags(const TextInputs& inputs, const RetainedStrings& strings, size_t index)
{
    const DecodedString& string = strings.decoded[index];
    uint32_t flags = 0;
    if (!hasHeader(inputs, strings, index))
        flags |= ImageString::hasNoHeaderFlag;
    if (getsHeaderWithCell(inputs, strings, index))
        flags |= ImageString::hasListedHashFlag;
    if (string.is16Bit())
        flags |= ImageString::is16BitFlag;
    if (inputs.uses.isAtom.get(index) || strings.isName.quickGet(index))
        flags |= ImageString::isAtomFlag;
    std::optional<uint32_t> identifierNumber = inputs.identifiers.numberOfText(StringID(index));
    if (!identifierNumber)
        return flags | string.hash;
    RELEASE_ASSERT(*identifierNumber <= ImageString::hashMask);
    return flags | ImageString::hasIdentifierNumberFlag | *identifierNumber;
}

[[nodiscard]] static bool encodeStringRecords(EncodedText& result, const TextInputs& inputs, const RetainedStrings& strings, const Vector<uint32_t>& ends, BuildContext& context)
{
    Vector<ImageString> records(FillWith { }, strings.decoded.size(), ImageString { 0, 0 });
    Vector<ImageLongString> longStrings { ImageLongString { 0, 0 } };
    for (size_t index : strings.isRetained) {
        const DecodedString& string = strings.decoded[index];
        uint32_t end = ends[strings.sharing.holders[index]];
        uint64_t distanceToEnd = static_cast<uint64_t>(strings.sharing.gaps[index]) + string.bytes().size();
        RELEASE_ASSERT(end > distanceToEnd);
        uint32_t offset = end - static_cast<uint32_t>(distanceToEnd);
        RELEASE_ASSERT(!string.is16Bit() || !(offset % sizeof(char16_t)));
        records[index] = ImageString { encodeLocation(offset, string.length(), hasHeader(inputs, strings, index), longStrings), encodeHashAndFlags(inputs, strings, index) };
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

static Vector<uint8_t> encodeIdentifiers(const TextInputs& inputs, std::span<const DecodedString> decoded)
{
    Vector<ImageIdentifier> records(inputs.identifiers.count(), [&](size_t number) {
        if (!number)
            return ImageIdentifier { 0, 0 };
        return encodeIdentifier(inputs.program, inputs.identifiers.at(static_cast<uint32_t>(number)), decoded);
    });
    return bytesOf(records);
}

static Vector<uint8_t> encodeStringConstants(const StringConstantNumbering& constants)
{
    Vector<uint32_t> indices(constants.count(), [&](size_t number) {
        return constants.stringOf(static_cast<uint32_t>(number)).index();
    });
    return bytesOf(indices);
}

EncodedText encodeText(const TextInputs& inputs, BuildContext& context)
{
    if (!checkSectionSize(static_cast<uint64_t>(inputs.program.strings.size()) * sizeof(DecodedString), context))
        return { };

    RetainedStrings strings;
    strings.isName = findNames(inputs);
    strings.isRetained = findRetainedStrings(inputs, strings.isName);
    strings.decoded = decodeRetainedStrings(inputs.program.strings, strings.isRetained, context);
    strings.sharing = shareText(strings.decoded.span(), strings.isRetained, strings.isName, context);
    context.statistics().count(hitsOf(LimitID::MaxComparedCharactersPerCharacter), strings.sharing.searchLimitHits);

    uint64_t maxTextSize = computeMaxTextSize(strings);
    if (!checkSectionSize(maxTextSize, context))
        return { };

    EncodedText result;
    result.text.reserveInitialCapacity(static_cast<size_t>(maxTextSize));
    Vector<uint32_t> ends = appendText(result.text, inputs, strings);
    if (!encodeStringRecords(result, inputs, strings, ends, context))
        return { };
    result.identifiers = encodeIdentifiers(inputs, strings.decoded.span());
    result.stringConstants = encodeStringConstants(inputs.constants);
    context.destroyLater(WTF::move(strings));
    return result;
}

unsigned EncodedText::hashOf(StringID id) const
{
    const ImageString& record = spanReinterpretCast<const ImageString>(strings.span())[id.index()];
    RELEASE_ASSERT(record.hasText());
    if (!record.hasIdentifierNumber())
        return record.hashOrIdentifierNumber();
    return spanReinterpretCast<const ImageIdentifier>(identifiers.span())[record.hashOrIdentifierNumber()].hashOrSymbol();
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
