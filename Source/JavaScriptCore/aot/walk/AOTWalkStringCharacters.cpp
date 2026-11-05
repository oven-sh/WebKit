/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

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
            return m_walker.constant(Known::number(PNaN));
        case CharacterReadInWalkStrings::CharAt:
            return m_walker.constant(Known::text(m_walker.program().strings.emptyString()));
        case CharacterReadInWalkStrings::CodePointAt:
        case CharacterReadInWalkStrings::At:
        case CharacterReadInWalkStrings::Element:
            return m_walker.constant(Known::undefined());
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

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
    Reference m_stringRead;
    Reference m_startOfPiece;
    Reference m_length;
    Origin m_origin;
    BlockID m_outside;
};

} // anonymous namespace

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

    std::array<Walker::ArmThatMeets, 2> arms { { { lastInside, { inside[0], inside[1] } }, { m_outside, { outside[0], outside[1] } } } };
    std::array<Reference, 2> met;
    if (m_walker.letArmsMeet(arms, m_walker.addBlock(), std::span<Reference>(met).first(count)))
        bindAndNote(met[0], met[1]);
}

bool BuilderOfCharacterRead::build(Reference position)
{
    TypeArena& types = m_walker.types();
    if (primitiveKnownForStrings(m_walker, position).kind() == Known::Kind::Undefined)
        position = integer(0);
    if (!position || !types.at(m_walker.typeOf(position)).isNumber())
        return false;

    if (std::optional<PieceOfString> piece = pieceInSight(m_walker, m_string)) {
        m_stringRead = piece->string;
        m_startOfPiece = piece->start;
        m_length = operate(Opcode::Sub, piece->end, piece->start);
    } else
        m_length = buildLengthOfString(m_walker, m_string, m_origin);
    if (!m_length)
        return true;

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

    bool isInside = type.integers.minimum >= 0 && (isBelowLength || nameProvesPositionBelowLengthForStrings(m_walker, position, m_string));
    isInside = isInside || (m_method != CharacterReadInWalkStrings::At && type.integers.minimum >= 0 && testOfBodyHoldsPositionOfCharacterReadBelowLength(m_walker, m_value));
    if (isInside && !m_walker.testsEveryPosition())
        m_walker.count(StatisticID::RangeChecksRemoved);
    if (!isInside) {
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

bool buildCharacterByIndex(Walker& walker, const BodyValue& value, const InputsOfTypedOperation& inputs)
{
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
    if (!read.build(call.arguments.empty() ? walker.constant(PrimitiveKnownToFolding::undefined()) : call.arguments[0]))
        return false;
    return true;
}

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

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
