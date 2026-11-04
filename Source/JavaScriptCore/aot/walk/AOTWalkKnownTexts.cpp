/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Folding: `const t = 'ab' + '中'; t.length`: the characters of a text are known where no constant may stand for it, because a constant would be held in another way than what the operation makes.
// At a suspension: a string never changes, so the note stays. It names no record.
// At a throw: the note is made behind the join, so no handler starts with the characters of a text that was not made.
// Withheld: nothing is noted, and only constants and literal types have characters.

#include "config.h"
#include "AOTWalkKnown.h"

#if ENABLE(AOT)

#include "AOTEffectsDeclared.h"
#include "AOTFoldingOfConstants.h"
#include "AOTIRProgram.h"
#include "AOTOpcodeSets.h"
#include "AOTStringTable.h"
#include "AOTTypeArena.h"
#include "JSString.h"
#include <wtf/text/WTFString.h>

namespace JSC { namespace AOT { namespace Typed {

// maxBytesOfTextMadeByFolding of AOTWalkFolding.cpp.
static constexpr size_t maxBytesOfCharactersInKnownTexts = 65536;

// primitiveHeldInFolding() of AOTWalkFolding.cpp: what a constant may stand for.
static PrimitiveKnownToFolding primitiveHeldInKnownTexts(Walker& walker, Reference reference)
{
    if (!reference)
        return { };
    if (reference.isRecord()) {
        const Operation& record = walker.code().at(reference.id());
        if (record.opcode == Opcode::Double)
            return PrimitiveKnownToFolding::number(record.number());
        if (record.opcode == Opcode::String)
            return PrimitiveKnownToFolding::text(record.string());
    }
    TypeID id = walker.typeOf(reference);
    if (!id)
        return { };
    Type type = walker.types().at(id);
    if (!type.isJavaScriptValue() || type.hasObjects())
        return { };
    switch (type.primitives) {
    case Primitive::Undefined:
        return PrimitiveKnownToFolding::undefined();
    case Primitive::Null:
        return PrimitiveKnownToFolding::null();
    case Primitive::False:
        return PrimitiveKnownToFolding::boolean(false);
    case Primitive::True:
        return PrimitiveKnownToFolding::boolean(true);
    case Primitive::Integer:
        return type.integers.isSingleValue() ? PrimitiveKnownToFolding::integer(type.integers.minimum) : PrimitiveKnownToFolding();
    case Primitive::LiteralString: {
        StringID literal = singleStringLiteralOf(type);
        return literal ? PrimitiveKnownToFolding::text(literal) : PrimitiveKnownToFolding();
    }
    default:
        return { };
    }
}

static StringID charactersNotedInKnownTexts(Walker& walker, Reference text)
{
    if (!text.isValue() || walker.state().answers().isEmpty())
        return { };
    std::optional<uint64_t> noted = findKnown(walker, KnownNote::CharactersOfText, text);
    return noted ? StringID::fromBits(static_cast<uint32_t>(*noted)) : StringID();
}

static PrimitiveKnownToFolding primitiveByCharactersInKnownTexts(Walker& walker, Reference reference)
{
    if (PrimitiveKnownToFolding held = primitiveHeldInKnownTexts(walker, reference))
        return held;
    StringID noted = charactersNotedInKnownTexts(walker, reference);
    return noted ? PrimitiveKnownToFolding::text(noted) : PrimitiveKnownToFolding();
}

StringID charactersKnownOf(Walker& walker, Reference text)
{
    PrimitiveKnownToFolding known = primitiveByCharactersInKnownTexts(walker, text);
    return known.isText() ? known.asText() : StringID();
}

bool isTextMadeByProgram(Walker& walker, Reference text)
{
    return !primitiveHeldInKnownTexts(walker, text) && charactersNotedInKnownTexts(walker, text);
}

void noteCharactersOf(Walker& walker, Reference text, StringID characters)
{
    if (text.isValue() && characters)
        noteKnown(walker, KnownNote::CharactersOfText, text, { }, 0, LocationMasks(), characters.bits());
}

namespace {

struct PartInKnownTexts {
    // Null: a number.
    StringID text;
    double number { 0 };
};

} // anonymous namespace

// Each part as ToString spells it: joinTextsInFolding(), with narrow parts beside wide ones listed too. False: a part is not known, or the text is too long.
// Nothing here goes through the bytes of a made text: in a row of joins each text is a part of the next.
static bool listPartsInKnownTexts(Walker& walker, std::span<const Reference> parts, size_t mostBytes, Vector<PartInKnownTexts, 8>& listed)
{
    const StringTable& strings = walker.program().strings;
    TypeArena& types = walker.types();
    size_t numberOfBytes = 0;
    for (Reference part : parts) {
        PrimitiveKnownToFolding known = primitiveByCharactersInKnownTexts(walker, part);
        StringID text;
        switch (known.kind()) {
        case PrimitiveKnownToFolding::Kind::Unknown:
            return false;
        case PrimitiveKnownToFolding::Kind::Undefined:
            text = strings.textOfUndefined();
            break;
        case PrimitiveKnownToFolding::Kind::Null:
            text = strings.textOfNull();
            break;
        case PrimitiveKnownToFolding::Kind::Boolean:
            text = strings.textOf(known.asBoolean());
            break;
        case PrimitiveKnownToFolding::Kind::Number:
            break;
        case PrimitiveKnownToFolding::Kind::Text:
            text = known.asText();
            break;
        }
        size_t numberOfBytesOfPart = text ? types.numberOfBytesOf(text) : String::number(known.asNumber()).length();
        numberOfBytes += numberOfBytesOfPart;
        if (numberOfBytes > mostBytes)
            return false;
        if (numberOfBytesOfPart)
            listed.append(PartInKnownTexts { text, text ? 0 : known.asNumber() });
    }
    return true;
}

void noteCharactersOfJoin(Walker& walker, const Draft& join, Reference joined)
{
    if (!joined.isValue() || !walker.has(Mechanism::Folding))
        return;
    TypeID type = walker.typeOf(joined);
    if (!type || !walker.types().at(type).isAnyString())
        return;
    Vector<PartInKnownTexts, 8> parts;
    if (!listPartsInKnownTexts(walker, join.operands(), maxBytesOfCharactersInKnownTexts, parts))
        return;
    TypeArena& types = walker.types();
    StringID characters;
    for (const PartInKnownTexts& part : parts) {
        StringID text = part.text ? part.text : types.internText(byteCast<uint8_t>(String::number(part.number).span8()));
        // Two halves of a pair that meet are one character, which is spelled in another way than the halves.
        if (characters && !types.joinedTextIsSpelledAsItsPieces(characters, text))
            return;
        characters = characters ? types.internJoinedTexts(characters, text) : text;
    }
    // A made text, though one part of the table has all the bytes.
    if (characters && !liesInThread(characters))
        characters = types.internText(types.bytesOf(characters));
    noteCharactersOf(walker, joined, characters ? characters : walker.program().strings.emptyString());
}

static Reference booleanInKnownTexts(Walker& walker, bool value)
{
    Draft constant(Opcode::Boolean);
    constant.setBoolean(value);
    return walker.constant(WTF::move(constant));
}

// Folding has answered for what constants and literal types decide, so one of the texts here is known by its note. No look at a loop knows the notes, and each has promised what the edges behind the test prove.
Reference answerKnownOfTexts(Walker& walker, const BodyValue& value)
{
    static constexpr OpcodeSet answered = allEqualities | OpcodeSet { Opcode::ToBoolean };
    Opcode opcode = value.opcode();
    if (!answered.contains(opcode) || walker.state().answers().isEmpty() || walker.isInLoop())
        return { };
    // The characters of a text are known of a constant or of what a join has made, which is proven a text.
    Reference first = walker.referenceOf(value.operand(0));
    if (walker.nothingIsProvenOf(first))
        return { };
    StringID left = charactersKnownOf(walker, first);
    if (!left)
        return { };
    TypeArena& types = walker.types();
    if (opcode == Opcode::ToBoolean)
        return booleanInKnownTexts(walker, !!types.numberOfBytesOf(left));
    StringID right = charactersKnownOf(walker, walker.referenceOf(value.operand(1)));
    return right ? booleanInKnownTexts(walker, types.spellSameText(left, right)) : Reference();
}

Reference lengthOfKnownCharacters(Walker& walker, Reference text)
{
    StringID characters = charactersNotedInKnownTexts(walker, text);
    if (!characters)
        return { };
    Vector<char16_t> buffer;
    Draft length(Opcode::Integer);
    length.setInteger(walker.types().viewOf(characters, buffer).length());
    return walker.constant(WTF::move(length));
}

std::optional<Effects> effectsOfJoinOfKnownTexts(Walker& walker, const Draft& join)
{
    if (join.opcode != Opcode::Strcat || !walker.has(Mechanism::Folding))
        return std::nullopt;
    // A text is held in no fewer bytes than it has characters. However long the parts are, so long as the join is not refused: a join that nobody reads must not stay for the sake of a refusal that never comes.
    Vector<PartInKnownTexts, 8> parts;
    if (!listPartsInKnownTexts(walker, join.operands(), JSString::MaxLength, parts))
        return std::nullopt;
    Effects effects = declaredEffectsOfOperation(walker.program(), Opcode::Strcat, join);
    if (effects.mayRunOtherCode())
        return std::nullopt;
    effects.mayThrow = ThrowEffect::onlyForLackOfMemory();
    return effects;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
