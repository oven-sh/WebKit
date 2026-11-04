/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Strings: `s[i]`, charAt, at, charCodeAt and codePointAt written out: the tests of the bounds, the read inside, the answer of the language outside.
// Beside a character or a char code that may lie outside meets its code unit, -1 outside: the two are parameters of one block, so the tie outlives a suspension and a throw as the two values do.
// The reads throw only for lack of memory. Withheld (BuiltinCalls for the methods, TypedOperations for `s[i]`): the call goes by its row, the read is the closed ElemGet.

#include "config.h"
#include "AOTWalkStrings.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDs.h"
#include "AOTFamilies.h"
#include "AOTIRProgram.h"
#include "AOTOpcodeSets.h"
#include "AOTTypeArena.h"
#include "AOTTypeTable.h"
#include "PureNaN.h"
#include <algorithm>
#include <array>
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

// How far behind a character read its mentions are looked for: O/walk/AOTWalkElementReads.cpp, InputUsesOfElementRead.
static constexpr uint32_t operationsSearchedBehindCharacterRead = 32;

namespace {

enum class CharacterReadInWalkStrings : uint8_t { CharCodeAt, CodePointAt, CharAt, At, Element };

class BuilderOfCharacterRead {
public:
    BuilderOfCharacterRead(Walker& walker, const BodyValue& value, CharacterReadInWalkStrings method, Reference string, Origin origin)
        : m_walker(walker)
        , m_value(value)
        , m_method(method)
        , m_string(string)
        , m_stringRead(string)
        , m_origin(origin)
    {
    }

    // True: what stands for the read is bound, or the walk stands nowhere. False: nothing was built.
    bool build(Reference position);

private:
    using Known = PrimitiveKnownToFolding;

    bool answersWithString() const { return m_method == CharacterReadInWalkStrings::CharAt || m_method == CharacterReadInWalkStrings::At || m_method == CharacterReadInWalkStrings::Element; }
    bool leavesCodeUnit() const { return answersWithString() || m_method == CharacterReadInWalkStrings::CharCodeAt; }

    Reference operate(Opcode opcode, Reference left, Reference right = { }) { return buildOperatorForStrings(m_walker, opcode, left, right, m_origin); }
    Reference integer(int64_t value) { return integerForStrings(m_walker, value); }

    Reference valueOutside()
    {
        switch (m_method) {
        case CharacterReadInWalkStrings::CharCodeAt:
            return constantForStrings(m_walker, Known::number(PNaN));
        case CharacterReadInWalkStrings::CharAt:
            return constantForStrings(m_walker, Known::text(m_walker.program().strings.emptyString()));
        case CharacterReadInWalkStrings::CodePointAt:
        case CharacterReadInWalkStrings::At:
        case CharacterReadInWalkStrings::Element:
            return constantForStrings(m_walker, Known::undefined());
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    // Ends the block on the test. The walk goes on where it passes, and every test that fails leads to one block.
    void goOnWhere(Reference test)
    {
        if (!test || m_walker.isUnreachable())
            return;
        BlockID passed = m_walker.addBlock();
        if (!m_outside)
            m_outside = m_walker.addBlock(BlockFrequency::Unlikely);
        m_walker.branch(test, passed, m_outside);
        m_walker.continueIn(passed);
    }

    Reference isSurrogate(Reference codeUnit, int64_t firstOfKind)
    {
        return operate(Opcode::StrictEq, operate(Opcode::BitAnd, codeUnit, integer(0xFC00)), integer(firstOfKind));
    }

    // `s.slice(a, b).charCodeAt(i)`: the characters are read where they lie, so a piece that nobody else reads is never cut.
    Reference codeUnitAt(Reference position)
    {
        return buildCodeUnitOfString(m_walker, m_stringRead, placeInStringRead(position), m_origin);
    }

    Reference placeInStringRead(Reference position)
    {
        if (!m_startOfPiece || m_startOfPiece == integer(0))
            return position;
        return position == integer(0) ? m_startOfPiece : operate(Opcode::Add, m_startOfPiece, position);
    }

    Reference countedFromEnd(Reference position);
    Reference codePointAt(Reference position);
    void bindOutside();
    void readInside(Reference position, std::array<Reference, 2>& values);
    void meetAndBind(const std::array<Reference, 2>& inside);
    void bindAndNote(Reference given, Reference codeUnit);

    Walker& m_walker;
    const BodyValue& m_value;
    CharacterReadInWalkStrings m_method;
    Reference m_string;
    // The string itself, or what it is a piece of from `m_startOfPiece` on.
    Reference m_stringRead;
    Reference m_startOfPiece;
    Reference m_length;
    Origin m_origin;
    BlockID m_outside;
};

} // anonymous namespace

// at() counts a negative position from the end.
Reference BuilderOfCharacterRead::countedFromEnd(Reference position)
{
    Type type = m_walker.types().at(m_walker.typeOf(position));
    if (type.integers.minimum >= 0)
        return position;
    if (type.integers.maximum < 0)
        return operate(Opcode::Add, m_length, position);

    std::array<Reference, 1> counted;
    Reference isNegative = operate(Opcode::Less, position, integer(0));
    bool isBuilt = buildArmsForStrings(m_walker, isNegative, BlockFrequency::Normal, [&](std::span<Reference> results) {
        results[0] = operate(Opcode::Add, m_length, buildProofBehindTestForStrings(m_walker, position, isNegative, OperandIndex::Less::left, m_origin));
    }, [&](std::span<Reference> results) {
        results[0] = position;
    }, counted);
    return isBuilt ? counted[0] : Reference();
}

// Two code units where a trail surrogate stands behind a lead surrogate, else one. `position` lies inside the string.
Reference BuilderOfCharacterRead::codePointAt(Reference position)
{
    Reference unit = codeUnitAt(position);
    if (!unit)
        return { };
    BlockID found = m_walker.addBlock();
    Reference codePoint = m_walker.addParameter(found, { }, m_walker.formOfParameter(m_walker.types().integer(Range { 0, 0x10FFFF }), { }));

    BlockID single = m_walker.addBlock();
    BlockID behindLead = m_walker.addBlock(BlockFrequency::Unlikely);
    m_walker.branch(isSurrogate(unit, 0xD800), behindLead, single);
    m_walker.continueIn(behindLead);
    Reference next = operate(Opcode::Add, position, integer(1));
    Reference hasSecond = operate(Opcode::Less, next, m_length);
    BlockID second = m_walker.addBlock(BlockFrequency::Unlikely);
    m_walker.branch(hasSecond, second, single);
    m_walker.continueIn(second);
    Reference trail = codeUnitAt(buildProofBehindTestForStrings(m_walker, next, hasSecond, OperandIndex::Less::left, m_origin));
    BlockID pair = m_walker.addBlock(BlockFrequency::Unlikely);
    m_walker.branch(isSurrogate(trail, 0xDC00), pair, single);
    m_walker.continueIn(pair);
    Reference high = operate(Opcode::LShift, operate(Opcode::BitAnd, unit, integer(0x3FF)), integer(10));
    Reference both = operate(Opcode::Add, high, operate(Opcode::BitAnd, trail, integer(0x3FF)));
    Reference combined = operate(Opcode::Add, both, integer(0x10000));
    if (combined)
        m_walker.jumpTo(found, singleElementSpan(combined));
    m_walker.continueIn(single);
    m_walker.jumpTo(found, singleElementSpan(unit));
    return m_walker.isUnreachable() ? Reference() : codePoint;
}

void BuilderOfCharacterRead::readInside(Reference position, std::array<Reference, 2>& values)
{
    if (m_method == CharacterReadInWalkStrings::CodePointAt) {
        values[0] = codePointAt(position);
        return;
    }
    values[1] = codeUnitAt(position);
    if (m_method == CharacterReadInWalkStrings::CharCodeAt) {
        values[0] = values[1];
        return;
    }
    Reference place = placeInStringRead(position);
    values[0] = buildPieceOfString(m_walker, m_stringRead, place, operate(Opcode::Add, place, integer(1)), m_origin);
}

void BuilderOfCharacterRead::bindAndNote(Reference given, Reference codeUnit)
{
    bindForStrings(m_walker, m_value.id(), given, m_origin);
    if (!given.isValue() || !codeUnit || given == codeUnit)
        return;
    NotesOfStrings& notes = notesOfStrings(m_walker);
    uint32_t record = given.id().index();
    if (answersWithString())
        notes.codeUnitsBesideCharacters.set(record, codeUnit);
    else
        notes.codeUnitsBesideCharCodes.set(record, codeUnit);
}

void BuilderOfCharacterRead::bindOutside()
{
    m_walker.bind(m_value.id(), valueOutside());
}

void BuilderOfCharacterRead::meetAndBind(const std::array<Reference, 2>& inside)
{
    size_t count = leavesCodeUnit() ? 2 : 1;
    BlockID lastInside = m_walker.isUnreachable() ? BlockID() : m_walker.currentBlock();
    if (m_walker.hasFailed())
        return;
    if (lastInside && (!inside[0] || (count == 2 && !inside[1])))
        return m_walker.fail("a character read has no value where the position lies inside the string"_s);
    if (!m_outside) {
        if (lastInside)
            bindAndNote(inside[0], inside[1]);
        return;
    }

    std::array<Reference, 2> outside { valueOutside(), integer(-1) };
    if (!lastInside) {
        m_walker.continueIn(m_outside);
        bindOutside();
        return;
    }

    const Code& code = m_walker.code();
    BlockID meeting = m_walker.addBlock();
    std::array<Reference, 2> met;
    for (size_t place = 0; place < count; ++place) {
        TypeID type = joinOf(m_walker.types(), m_walker.facts().families(), code.at(inside[place].id()).type, code.at(outside[place].id()).type);
        Representation forms[] { m_walker.representationOf(inside[place]), m_walker.representationOf(outside[place]) };
        met[place] = m_walker.addParameter(meeting, type, m_walker.formOfParameter(type, forms));
    }
    m_walker.jumpTo(meeting, std::span<const Reference>(inside).first(count));
    m_walker.continueIn(m_outside);
    m_walker.jumpTo(meeting, std::span<const Reference>(outside).first(count));
    m_walker.continueIn(meeting);
    if (!m_walker.isUnreachable() && !m_walker.hasFailed())
        bindAndNote(met[0], met[1]);
}

bool BuilderOfCharacterRead::build(Reference position)
{
    TypeArena& types = m_walker.types();
    if (primitiveKnownForStrings(m_walker, position).kind() == Known::Kind::Undefined)
        position = integer(0);
    if (!position || !types.at(m_walker.typeOf(position)).isNumber())
        return false;

    // `for (const ch of text) ch.codePointAt(0)`: the step has read it.
    if (m_method == CharacterReadInWalkStrings::CodePointAt && primitiveKnownForStrings(m_walker, position).kind() == Known::Kind::Number && !primitiveKnownForStrings(m_walker, position).asNumber()) {
        Reference piece = rootForStrings(m_walker, m_string);
        if (piece.isValue()) {
            auto& stepped = notesOfStrings(m_walker).codePointsOfSteppedPieces;
            if (auto noted = stepped.find(piece.id().index()); noted != stepped.end()) {
                bindForStrings(m_walker, m_value.id(), noted->value.codePoint, m_origin);
                return true;
            }
        }
    }

    // Of a piece whose end lies in front of its start the difference is negative, and no position lies below it: the piece is empty.
    if (std::optional<PieceOfString> piece = pieceInSight(m_walker, m_string)) {
        m_stringRead = piece->string;
        m_startOfPiece = piece->start;
        m_length = operate(Opcode::Sub, piece->end, piece->start);
    } else
        m_length = buildLengthOfString(m_walker, m_string, m_origin);
    if (!m_length)
        return true;

    // Whatever is no int32 behind ToIntegerOrInfinity lies outside every string.
    if (!types.at(m_walker.typeOf(position)).isInteger()) {
        Reference root = rootForStrings(m_walker, position);
        if (!root.isValue() || m_walker.code().at(root.id()).opcode != Opcode::ToIntegerOrInfinity)
            position = operate(Opcode::ToIntegerOrInfinity, position);
        Reference isInt32 = buildTypeTestForStrings(m_walker, position, TypeTable::int32, m_origin);
        if (!isInt32)
            return true;
        if (Known decided = primitiveKnownForStrings(m_walker, isInt32); decided.kind() == Known::Kind::Boolean && !decided.asBoolean()) {
            bindOutside();
            return true;
        }
        goOnWhere(isInt32);
        position = buildProofBehindTestForStrings(m_walker, position, isInt32, OperandIndex::TypeTest::value, m_origin);
        if (position && !types.at(m_walker.typeOf(position)).isInteger())
            position = operate(Opcode::ToInt32, position);
        if (!position)
            return true;
    }

    if (m_method == CharacterReadInWalkStrings::At)
        position = countedFromEnd(position);
    if (!position)
        return true;

    Type type = types.at(m_walker.typeOf(position));
    std::optional<bool> isBelowLength = relationDecidedForStrings(m_walker, Opcode::Less, position, m_length);
    if (type.integers.maximum < 0 || (isBelowLength && !*isBelowLength)) {
        if (!m_outside) {
            bindOutside();
            return true;
        }
        m_walker.jumpTo(m_outside);
        bindOutside();
        return true;
    }

    bool isInside = type.integers.minimum >= 0 && (isBelowLength || nameProvesPositionBelowLengthForStrings(m_walker, position, m_string) || testThatLedHereProvesPositionBelowLength(m_walker, m_value, m_string, position));
    // at() may have counted from the end, and reads at another value than the test has seen.
    isInside = isInside || (m_method != CharacterReadInWalkStrings::At && type.integers.minimum >= 0 && testOfBodyHoldsPositionOfCharacterReadBelowLength(m_walker, m_value));
    if (!isInside) {
        // As an unsigned word a negative int32 lies behind every length.
        if (type.integers.minimum < 0 && type.isInt32())
            position = operate(Opcode::URShift, position, integer(0));
        else if (type.integers.minimum < 0) {
            Reference isNotNegative = operate(Opcode::LessEq, integer(0), position);
            goOnWhere(isNotNegative);
            position = buildProofBehindTestForStrings(m_walker, position, isNotNegative, OperandIndex::LessEq::right, m_origin);
        }
        Reference isBelow = operate(Opcode::Less, position, m_length);
        goOnWhere(isBelow);
        position = buildProofBehindTestForStrings(m_walker, position, isBelow, OperandIndex::Less::left, m_origin);
    }

    std::array<Reference, 2> inside;
    if (position && !m_walker.isUnreachable())
        readInside(position, inside);
    meetAndBind(inside);
    return true;
}

// ---- The doors

// Outside the text the read asks the prototypes, from String.prototype upward. The run time refuses every index on them (ImmutablePropertyNames::Part::NumericKeys), but for a name that a test run has opened.
static bool indexesAreAbsentAboveStringsForStringCharacters(const Walker& walker)
{
    const Program& program = walker.program();
    const BuiltinTable& builtins = program.builtins();
    BuiltinID holder = builtins.canonicalID(Builtins::StringPrototype);
    while (true) {
        if (!builtins.closesIndexKeys(holder) || !builtins.hasLockedPrototypeLink(holder) || program.isSomeNameOpenedOn(holder))
            return false;
        const Builtin& object = builtins.at(holder);
        switch (object.prototypeLink) {
        case BuiltinPrototypeLink::Unknown:
            return false;
        case BuiltinPrototypeLink::Null:
            return true;
        case BuiltinPrototypeLink::Builtin:
            holder = builtins.canonicalID(object.inheritsFrom);
            break;
        }
    }
}

// A mention of the character that its code unit answers as well: a test of its type, an equality with a written text, undefined or null, a relation with a written text of one code unit.
static bool asksWhatCodeUnitAnswersForStringCharacters(const BodyIndex& index, const BodyValue& user, uint32_t operandIndex)
{
    Opcode opcode = user.opcode();
    if (opcode == Opcode::TypeTest)
        return true;
    bool isEquality = allEqualities.contains(opcode);
    if (!isEquality && !allRelations.contains(opcode))
        return false;
    ValueID other = index.valueBehindCasts(user.operand(1 - operandIndex));
    if (!other || index.isParameter(other))
        return false;
    switch (index.opcodeOf(other)) {
    case Opcode::String:
        return true;
    case Opcode::Undefined:
    case Opcode::Null:
        return isEquality;
    default:
        return false;
    }
}

// Every mention of the value stands in its block, close behind it, and asks what the code unit answers.
static bool isOnlyAskedAboutForStringCharacters(Walker& walker, ValueID character)
{
    const BodyIndex& index = walker.currentBodyIndex();
    if (index.isUnused(character))
        return false;
    Vector<ValueID, 4> names { character };
    uint32_t mentionsLeft = index.useCount(character);
    if (mentionsLeft == BodyIndex::maxCountedUses)
        return false;

    uint32_t end = std::min(index.block(index.blockOf(character)).endOfValues().index(), character.index() + 1 + operationsSearchedBehindCharacterRead);
    bool everyMentionAsks = true;
    for (uint32_t place = character.index() + 1; place < end && mentionsLeft && everyMentionAsks; ++place) {
        BodyValue user = index.value(ValueID(place));
        bool handsOn = false;
        user.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
            if (!names.contains(operand))
                return;
            --mentionsLeft;
            if (opcodesThatStateTypes.contains(user.opcode()) && !operandIndex)
                handsOn = true;
            else if (!asksWhatCodeUnitAnswersForStringCharacters(index, user, operandIndex))
                everyMentionAsks = false;
        });
        if (!handsOn)
            continue;
        uint32_t mentions = index.useCount(user.id());
        if (mentions == BodyIndex::maxCountedUses)
            return false;
        names.append(user.id());
        mentionsLeft += mentions;
    }
    return everyMentionAsks && !mentionsLeft;
}

static void noteWhetherCharacterIsOnlyAskedAbout(Walker& walker, const BodyValue& value)
{
    Reference character = walker.isUnreachable() ? Reference() : rootForStrings(walker, walker.referenceBoundTo(value.id()));
    if (character.isValue() && notesOfStrings(walker).codeUnitsBesideCharacters.contains(character.id().index()) && isOnlyAskedAboutForStringCharacters(walker, value.id()))
        notesOfStrings(walker).charactersOnlyAskedAbout.add(character.id().index());
}

bool buildCharacterByIndex(Walker& walker, const BodyValue& value, const InputsOfTypedOperation& inputs)
{
    // -0 names the first character and is no integer.
    if (!isProvenString(walker, inputs.typeOf(value.objectOperand())) || !walker.types().at(inputs.typeOf(value.propertyOperand())).isInteger())
        return false;
    if (!indexesAreAbsentAboveStringsForStringCharacters(walker))
        return false;
    if (inputs.areOnlyTypes()) {
        walker.noteFormAskedAhead(Opcode::Substring, KnowledgeLevel::Proven);
        return true;
    }
    if (walker.isUnreachable())
        return false;

    Origin origin = walker.originOf(value);
    origin.isTailCall = false;
    BuilderOfCharacterRead read(walker, value, CharacterReadInWalkStrings::Element, walker.referenceOf(value.objectOperand()), origin);
    if (!read.build(walker.referenceOf(value.propertyOperand())))
        return false;
    noteWhetherCharacterIsOnlyAskedAbout(walker, value);
    return true;
}

bool buildCharacterReadByMethod(Walker& walker, const CallByOneRow& call)
{
    if (!call.receiver || call.arguments.size() > 1 || !isProvenString(walker, walker.typeOf(call.receiver)))
        return false;

    CharacterReadInWalkStrings method;
    switch (*call.codeInLine) {
    case InlineBuiltin::StringCharCodeAt:
        method = CharacterReadInWalkStrings::CharCodeAt;
        break;
    case InlineBuiltin::StringCodePointAt:
        method = CharacterReadInWalkStrings::CodePointAt;
        break;
    case InlineBuiltin::StringCharAt:
        method = CharacterReadInWalkStrings::CharAt;
        break;
    case InlineBuiltin::StringAt:
        method = CharacterReadInWalkStrings::At;
        break;
    default:
        return false;
    }
    BuilderOfCharacterRead read(walker, call.call, method, call.receiver, call.origin);
    if (!read.build(call.arguments.empty() ? constantForStrings(walker, PrimitiveKnownToFolding::undefined()) : call.arguments[0]))
        return false;
    noteWhetherCharacterIsOnlyAskedAbout(walker, call.call);
    return true;
}

// ---- What a read has left

Reference codeUnitOfCharacterInSight(Walker& walker, Reference character)
{
    Reference root = rootForStrings(walker, character);
    if (!root.isValue() || !walker.types().at(walker.typeOf(root)).hasOnlyPrimitives(Primitive::AnyString | Primitive::Undefined))
        return { };
    return notesOfStrings(walker).codeUnitsBesideCharacters.get(root.id().index());
}

Reference codeUnitOfCharCodeInSight(Walker& walker, Reference number)
{
    Reference root = rootForStrings(walker, number);
    if (!root.isValue() || !walker.types().at(walker.typeOf(root)).isNumber())
        return { };
    return notesOfStrings(walker).codeUnitsBesideCharCodes.get(root.id().index());
}

void noteCodePointOfSteppedPiece(Walker& walker, Reference piece, Reference codePoint, Reference numberOfCodeUnits)
{
    Reference root = rootForStrings(walker, piece);
    if (!root.isValue() || !codePoint || !numberOfCodeUnits)
        return;
    notesOfStrings(walker).codePointsOfSteppedPieces.set(root.id().index(), NotesOfStrings::CodePointOfPiece { codePoint, numberOfCodeUnits });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
