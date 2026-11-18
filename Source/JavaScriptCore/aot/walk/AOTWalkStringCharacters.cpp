/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalkStrings.h"
#include "AOTWalkTextBuilders.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDs.h"
#include "AOTEffectsDeclared.h"
#include "AOTFamilies.h"
#include "AOTIRProgram.h"
#include "AOTOpcodeSets.h"
#include "AOTTypeArena.h"
#include "AOTTypeTable.h"
#include "AOTWalkNumberForms.h"
#include "AOTWalkNumberGuesses.h"
#include "PureNaN.h"
#include <algorithm>
#include <array>
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

Reference boxNumber(Walker&, Reference, Origin);


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
    bool isReadForBits() const { return m_method == CharacterReadInWalkStrings::CharCodeAt && findTruncatedUse(m_walker, m_value.id()) == TruncatedUse::OperandOfBits; }
    bool leavesCodeUnit() const { return answersWithString() || m_method == CharacterReadInWalkStrings::CharCodeAt; }

    bool mayRestartIteration() const { return m_walker.canRestartIteration() && m_walker.opensGuesses(); }
    bool restartsIterationOutside() const { return !m_outside && mayRestartIteration() && m_method != CharacterReadInWalkStrings::CharAt && !m_isOutsideAtFirst; }
    Reference operate(Opcode opcode, Reference left, Reference right = { })
    {
        Walker::SpecializedPreheaderScope preheaderScope(m_walker, m_isReadInFront);
        return buildOperatorForStrings(m_walker, opcode, left, right, m_origin);
    }
    Reference proveBehind(Reference value, Reference test, uint32_t operandIndex)
    {
        Walker::SpecializedPreheaderScope preheaderScope(m_walker, m_isReadInFront);
        return buildProofBehindTestForStrings(m_walker, value, test, operandIndex, m_origin);
    }
    Reference integer(int64_t value) { return integerForStrings(m_walker, value); }
    bool textMayBeReadInFront()
    {
        Reference flat = flatStringInSight(m_walker, m_stringRead);
        if (flat && m_walker.isInvariantInSpecializedVersion(flat))
            return true;
        return m_walker.tryHoistEntryTest() || !flat;
    }

    Reference valueOutside()
    {
        switch (m_method) {
        case CharacterReadInWalkStrings::CharCodeAt:
            if (isReadForBits()) {
                m_walker.count(StatisticID::CharacterCodesReadForBits);
                return integer(0);
            }
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

    void continueIf(Reference test)
    {
        if (!test || m_walker.isUnreachable())
            return;
        if (restartsIterationOutside()) {
            m_isReadInFront = m_isReadInFront && m_walker.tryBranchInPreheader(test, true);
            if (!m_isReadInFront) {
                BlockID inside = m_walker.addBlock();
                BlockID leaving = m_walker.addBlock(BlockFrequency::Rare);
                m_walker.letGuessFailUnasked(leaving, m_origin);
                m_walker.branch(test, inside, leaving);
                m_walker.continueIn(leaving);
                if (!m_walker.isUnreachable())
                    m_walker.restartIteration();
                m_walker.continueIn(inside);
            }
            m_walker.recordSpeculation();
            return;
        }
        m_isReadInFront = false;
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
        Reference place = placeInStringRead(position);
        Walker::SpecializedPreheaderScope preheaderScope(m_walker, m_isReadInFront);
        Reference flat = m_isInsideByItsCounter && !m_startOfPiece ? flatStringForReaderOfCharacters(m_walker, m_stringRead, m_origin, true) : Reference();
        return buildCodeUnitOfString(m_walker, flat ? flat : m_stringRead, place, m_origin);
    }

    Reference truncatedQuotient()
    {
        if (m_value.opcode() != Opcode::Call || m_walker.isUnreachable())
            return { };
        auto arguments = m_value.as<Opcode::Call>().argumentsOperands();
        if (!arguments.size())
            return { };
        std::optional<DivisionAsWritten> division = findQuotientOfIntegers(m_walker, *arguments.begin(), InputsOfTypedOperation(m_walker, nullptr));
        return division ? buildDivisionOfIntegers(m_walker, m_origin, division->dividend, division->divisor, QuotientRounding::Truncate) : Reference();
    }

    Reference textOfCodeUnit(Reference codeUnit)
    {
        const BuiltinTable& table = m_walker.program().builtins();
        for (BuiltinOverloadID id : table.overloadIDsOf(table.canonicalID(Builtins::StringFromCharCode))) {
            const BuiltinOverload& overload = table.at(id);
            if (overload.mode != BuiltinCallMode::Call || overload.code.implementation.inlineBuiltin != InlineBuiltin::StringFromCharCode)
                continue;
            Draft call(Opcode::CallInline, m_origin);
            call.setInlineBuiltin(InlineBuiltin::StringFromCharCode);
            call.setOverload(id);
            call.setArgumentsOperands(std::array { codeUnit });
            if (m_walker.isUnreachable())
                return { };
            m_walker.chooseConservativeFields(call);
            Effects ofPiece = declaredBoundInTable(Opcode::Substring);
            return m_walker.build(WTF::move(call), &ofPiece);
        }
        return { };
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
    static constexpr int64_t mostOutsideAtFirst = 64;
    bool m_isOutsideAtFirst { false };
    bool m_isReadInFront { false };
    bool m_isInsideByItsCounter { false };
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
    values[0] = values[1] && m_walker.isInLoop() ? textOfCodeUnit(values[1]) : Reference();
    if (values[0] || m_walker.isUnreachable())
        return;
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
    if (m_walker.keepsLogsForUndo())
        notes.codeUnitsChanged.append({ record, (answersWithString() ? notes.codePointsBesideCharacters : notes.codeUnitsBesideCharCodes).get(record), answersWithString() });
    if (answersWithString())
        notes.codePointsBesideCharacters.set(record, codeUnit);
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

    std::array<Reference, 2> outside { valueOutside(), integer(isReadForBits() ? 0 : -1) };
    if (!lastInside) {
        m_walker.continueIn(m_outside);
        bindOutside();
        return;
    }

    Reference given = inside[0];
    if (m_method == CharacterReadInWalkStrings::CharCodeAt && !m_walker.isInLoop() && m_walker.representationOf(given) == Representation::Int32)
        given = boxNumber(m_walker, given, m_origin);
    if (given != inside[0])
        m_walker.count(StatisticID::CharacterCodesKeptBoxed);
    if (m_walker.hasFailed() || m_walker.isUnreachable())
        return;
    std::array<Walker::JoinArm, 2> arms { { { lastInside, { given, inside[1] } }, { m_outside, { outside[0], outside[1] } } } };
    std::array<Reference, 2> met;
    if (m_walker.joinArms(arms, m_walker.addBlock(), std::span<Reference>(met).first(count)))
        bindAndNote(met[0], met[1]);
}

bool BuilderOfCharacterRead::build(Reference position)
{
    TypeArena& types = m_walker.types();
    if (primitiveKnownForStrings(m_walker, position).kind() == Known::Kind::Undefined)
        position = integer(0);
    if (!position || !types.at(m_walker.typeOf(position)).isNumber())
        return false;

    bool asksForNumber = m_method == CharacterReadInWalkStrings::CharCodeAt || m_method == CharacterReadInWalkStrings::CodePointAt;
    Known place = primitiveKnownForStrings(m_walker, position);
    Reference beside = asksForNumber && place.kind() == Known::Kind::Number && !place.asNumber() ? codePointOfCharacterInSight(m_walker, m_string) : Reference();
    if (beside && types.at(m_walker.typeOf(beside)).isInteger() && types.at(m_walker.typeOf(beside)).integers.minimum >= 0) {
        Reference read = beside;
        if (m_method == CharacterReadInWalkStrings::CharCodeAt && types.at(m_walker.typeOf(beside)).integers.maximum > 0xFFFF) {
            std::array<Reference, 1> unit;
            bool isBuilt = buildArmsForStrings(m_walker, operate(Opcode::Less, beside, integer(0x10000)), BlockFrequency::Unlikely, [&](std::span<Reference> results) {
                results[0] = operate(Opcode::BitAnd, beside, integer(0xFFFF));
            }, [&](std::span<Reference> results) {
                Reference bits = operate(Opcode::BitAnd, operate(Opcode::RShift, operate(Opcode::Sub, beside, integer(0x10000)), integer(10)), integer(0x3FF));
                results[0] = operate(Opcode::Add, bits, integer(0xD800));
            }, unit);
            if (!isBuilt)
                return true;
            read = unit[0];
        }
        bindForStrings(m_walker, m_value.id(), read, m_origin);
        return true;
    }

    std::optional<PieceOfString> piece = pieceReadInLoop(m_walker, m_string, m_origin);
    if (!piece)
        piece = pieceInSight(m_walker, m_string);
    if (piece) {
        m_stringRead = piece->string;
        m_startOfPiece = piece->start;
        m_length = operate(Opcode::Sub, piece->end, piece->start);
    } else
        m_length = buildLengthOfString(m_walker, m_string, m_origin);
    if (!m_length)
        return true;

    if (m_method == CharacterReadInWalkStrings::Element && !types.at(m_walker.typeOf(position)).isInteger()) {
        Reference truncated = operate(Opcode::ToInt32, position);
        Reference isIndex = truncated ? operate(Opcode::StrictEq, truncated, position) : Reference();
        if (!isIndex)
            return true;
        if (Known decided = primitiveKnownForStrings(m_walker, isIndex); decided.kind() == Known::Kind::Boolean && !decided.asBoolean()) {
            bindOutside();
            return true;
        }
        continueIf(isIndex);
        position = truncated;
    } else if (Reference quotient = types.at(m_walker.typeOf(position)).isInteger() ? Reference() : truncatedQuotient(); quotient && types.at(m_walker.typeOf(quotient)).isInteger())
        position = quotient;
    else if (!types.at(m_walker.typeOf(position)).isInteger()) {
        Reference root = rootForStrings(m_walker, position);
        bool isAskedByTag = position.isValue() && m_walker.representationOf(position) == Representation::JSValue && restartsIterationOutside();
        if (isAskedByTag) {
            Reference hasTag = buildTypeTestForStrings(m_walker, position, TypeTable::int32, m_origin);
            if (!hasTag)
                return true;
            Known answer = primitiveKnownForStrings(m_walker, hasTag);
            isAskedByTag = answer.kind() != Known::Kind::Boolean || answer.asBoolean();
        }
        if (!isAskedByTag && (!root.isValue() || m_walker.code().at(root.id()).opcode != Opcode::ToIntegerOrInfinity))
            position = operate(Opcode::ToIntegerOrInfinity, position);
        Reference isInt32 = buildTypeTestForStrings(m_walker, position, TypeTable::int32, m_origin);
        if (!isInt32)
            return true;
        if (Known decided = primitiveKnownForStrings(m_walker, isInt32); decided.kind() == Known::Kind::Boolean && !decided.asBoolean()) {
            bindOutside();
            return true;
        }
        continueIf(isInt32);
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
    m_isOutsideAtFirst = type.isInteger() && type.integers.minimum < 0 && type.integers.minimum >= -mostOutsideAtFirst;
    if (m_method == CharacterReadInWalkStrings::Element && mayRestartIteration() && !m_isOutsideAtFirst && readsCharacterAtOuterCounter(m_walker, m_value)) {
        m_isOutsideAtFirst = true;
        m_walker.count(StatisticID::CharactersReadPastLengths);
    }
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
    m_isInsideByItsCounter = !isInside && m_method != CharacterReadInWalkStrings::At && type.integers.minimum >= 0
        && (readsCharacterUnderLoweredCounter(m_walker, m_value) || (m_walker.isInLoop() && readsCharacterBeforeTestedNumber(m_walker, m_value)));
    isInside = isInside || m_isInsideByItsCounter;
    if (isInside && !m_walker.checksBounds())
        m_walker.count(StatisticID::RangeChecksRemoved);
    if (!isInside) {
        bool isUnitAtPosition = m_method == CharacterReadInWalkStrings::CharCodeAt || m_method == CharacterReadInWalkStrings::Element;
        m_isReadInFront = isUnitAtPosition && !m_outside && mayRestartIteration() && m_walker.isInvariantInSpecializedVersion(position)
            && m_walker.isInvariantInSpecializedVersion(m_length) && m_walker.isInvariantInSpecializedVersion(m_stringRead) && (!m_startOfPiece || m_walker.isInvariantInSpecializedVersion(m_startOfPiece))
            && textMayBeReadInFront();
        if (type.integers.minimum < 0 && type.isInt32())
            position = operate(Opcode::URShift, position, integer(0));
        else if (type.integers.minimum < 0) {
            Reference isNotNegative = operate(Opcode::LessEq, integer(0), position);
            continueIf(isNotNegative);
            position = proveBehind(position, isNotNegative, OperandIndex::LessEq::right);
        }
        Reference isBelow = operate(Opcode::Less, position, m_length);
        continueIf(isBelow);
        position = proveBehind(position, isBelow, OperandIndex::Less::left);
    }
    if (position && !m_walker.isUnreachable() && !types.at(m_walker.typeOf(position)).isInt32())
        position = operate(Opcode::ToInt32, position);

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
        if (!builtins.closesIndexKeys(holder) || !builtins.hasLockedPrototypeLink(holder))
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
    Type key = walker.types().at(inputs.typeOf(value.propertyOperand()));
    bool standsInLoop = walker.isInLoop() || walker.currentBodyIndex().loopDepthOf(walker.currentBodyIndex().blockOf(value.id()));
    if (!isProvenString(walker, inputs.typeOf(value.objectOperand())) || !(standsInLoop ? key.isNumber() : key.isInteger()))
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

static std::optional<CharacterReadInWalkStrings> characterReadByMethod(InlineBuiltin method)
{
    switch (method) {
    case InlineBuiltin::StringCharCodeAt:
        return CharacterReadInWalkStrings::CharCodeAt;
    case InlineBuiltin::StringCodePointAt:
        return CharacterReadInWalkStrings::CodePointAt;
    case InlineBuiltin::StringCharAt:
        return CharacterReadInWalkStrings::CharAt;
    case InlineBuiltin::StringAt:
        return CharacterReadInWalkStrings::At;
    default:
        return std::nullopt;
    }
}

bool readsCharacterOfText(InlineBuiltin method)
{
    return !!characterReadByMethod(method);
}

static bool readsFirstCharacter(Walker& walker, const CallByOneRow& call)
{
    if (call.arguments.empty())
        return true;
    TypeID position = call.arguments[0] && call.arguments[0].isRecord() ? walker.typeOf(call.arguments[0]) : TypeID();
    if (!position)
        return false;
    Type type = walker.types().at(position);
    return type.isInteger() && type.integers.isSubsetOf({ 0, 0 });
}

bool buildCharacterReadByMethod(Walker& walker, const CallByOneRow& call)
{
    std::optional<CharacterReadInWalkStrings> method = characterReadByMethod(*call.codeInLine);
    if (!method || !call.receiver || call.arguments.size() > 1 || !isProvenString(walker, walker.typeOf(call.receiver)))
        return false;
    if (*method == CharacterReadInWalkStrings::CodePointAt && (!walker.isInLoop() || (readsFirstCharacter(walker, call) && !codePointOfCharacterInSight(walker, call.receiver))))
        return false;

    BuilderOfCharacterRead read(walker, call.call, *method, call.receiver, call.origin);
    if (!read.build(call.arguments.empty() ? walker.constant(PrimitiveKnownToFolding::undefined()) : call.arguments[0]))
        return false;
    return true;
}

void noteCodePointBesideCharacter(Walker& walker, Reference character, Reference codePoint)
{
    if (walker.isUnreachable() || !character || !character.isValue() || !codePoint)
        return;
    NotesOfStrings& notes = notesOfStrings(walker);
    uint32_t record = character.id().index();
    if (walker.keepsLogsForUndo())
        notes.codeUnitsChanged.append({ record, notes.codePointsBesideCharacters.get(record), true });
    notes.codePointsBesideCharacters.set(record, codePoint);
}

Reference codePointOfCharacterInSight(Walker& walker, Reference character)
{
    Reference root = rootForStrings(walker, character);
    if (!root.isValue() || !walker.types().at(walker.typeOf(root)).hasOnlyPrimitives(Primitive::AnyString | Primitive::Undefined))
        return { };
    return notesOfStrings(walker).codePointsBesideCharacters.get(root.id().index());
}

Reference buildOnlyCharacter(Walker& walker, Reference string, Origin origin)
{
    Reference codeUnit = buildCodeUnitOfString(walker, string, integerForStrings(walker, 0), origin);
    const BuiltinTable& table = walker.program().builtins();
    for (BuiltinOverloadID id : table.overloadIDsOf(table.canonicalID(Builtins::StringFromCharCode))) {
        const BuiltinOverload& overload = table.at(id);
        if (overload.mode != BuiltinCallMode::Call || overload.code.implementation.inlineBuiltin != InlineBuiltin::StringFromCharCode)
            continue;
        if (!codeUnit || !codeUnit.isValue() || walker.isUnreachable() || walker.hasFailed())
            return { };
        Draft call(Opcode::CallInline, origin);
        call.setInlineBuiltin(InlineBuiltin::StringFromCharCode);
        call.setOverload(id);
        call.setArgumentsOperands(std::array { codeUnit });
        walker.chooseConservativeFields(call);
        Effects ofPiece = declaredBoundInTable(Opcode::Substring);
        Reference character = walker.build(WTF::move(call), &ofPiece);
        if (character && character.isValue())
            noteCodePointBesideCharacter(walker, character, codeUnit);
        return character;
    }
    return { };
}

static constexpr size_t maxStringsMeasuredPerJoin = 2;
static constexpr size_t mostIntegersMeasuredForOneJoin = 4;
static constexpr int64_t greatestIntegerMeasuredForJoin = 99999;

Reference buildLengthOfCharacterInSight(Walker& walker, Reference character, Origin origin)
{
    static constexpr int64_t greatestCodeUnit = std::numeric_limits<char16_t>::max();
    if (walker.isUnreachable() || !character || !character.isValue() || !isProvenString(walker, walker.typeOf(character)))
        return { };
    Reference codePoint = codePointOfCharacterInSight(walker, character);
    Type type = codePoint ? walker.types().at(walker.typeOf(codePoint)) : Type();
    if (!codePoint || !type.isInteger() || type.integers.isEmpty() || type.integers.minimum < 0)
        return { };
    if (type.integers.maximum <= greatestCodeUnit)
        return integerForStrings(walker, 1);
    if (!walker.isInLoop())
        return { };
    Reference isCodeUnit = buildOperatorForStrings(walker, Opcode::LessEq, codePoint, integerForStrings(walker, greatestCodeUnit), origin);
    if (!isCodeUnit)
        return { };
    Reference met;
    auto one = [&](std::span<Reference> results) { results[0] = integerForStrings(walker, 1); };
    auto two = [&](std::span<Reference> results) { results[0] = integerForStrings(walker, 2); };
    return buildArmsForStrings(walker, isCodeUnit, BlockFrequency::Unlikely, scopedLambda<void(std::span<Reference>)>(one), scopedLambda<void(std::span<Reference>)>(two), singleElementSpan(met)) ? met : Reference();
}

static Reference integerConvertedToString(Walker& walker, Reference string)
{
    Reference root = rootOfStringForStrings(walker, string);
    if (!root.isValue())
        return { };
    const Operation& made = walker.code().at(root.id());
    Reference integer;
    auto takePart = [&](ValueID record) {
        Reference part = referenceToRecordForStrings(walker, record);
        if (StringID text = textKnownForStrings(walker, part))
            return !lengthOfTextForStrings(walker, text);
        if (integer)
            return false;
        Reference made = rootForStrings(walker, part);
        integer = made.isValue() && walker.code().at(made.id()).opcode == Opcode::Box ? referenceToRecordForStrings(walker, walker.code().at(made.id()).valueOperand()) : part;
        return true;
    };
    switch (made.opcode) {
    case Opcode::ToString:
        if (!takePart(made.operand(0)))
            return { };
        break;
    case Opcode::Add:
        if (!takePart(made.operand(0)) || !takePart(made.operand(1)))
            return { };
        break;
    case Opcode::Strcat:
        if (!std::ranges::all_of(made.partsOperands(), takePart))
            return { };
        break;
    default:
        return { };
    }
    return integer;
}

bool buildMeasuredIntegerString(Walker& walker, const CallByOneRow& call)
{
    if (walker.isUnreachable() || !walker.isInLoop() || call.isForwarded || call.call.opcode() != Opcode::Call)
        return false;
    bool isCallOfConstructor = *call.codeInLine == InlineBuiltin::CallStringConstructor;
    if (call.arguments.size() != (isCallOfConstructor ? 1 : 0))
        return false;
    Reference integer = isCallOfConstructor ? call.arguments[0] : call.receiver;
    Type type = integer ? walker.types().at(walker.typeOf(integer)) : Type();
    if (!integer || !integer.isValue() || !type.isInt32() || type.integers.isEmpty() || type.integers.minimum < 0)
        return false;
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID string = call.call.id();
    ValueID reader = index.onlyUsingValue(string);
    while (reader && opcodesThatStateTypes.contains(index.opcodeOf(reader))) {
        string = reader;
        reader = index.onlyUsingValue(reader);
    }
    if (!reader || index.opcodeOf(reader) != Opcode::PropGet || index.value(reader).isFreeName() || index.value(reader).key() != walker.program().keyOf(BuiltinNames::length) || index.value(reader).objectOperand() != string)
        return false;
    Reference built = buildOperatorForStrings(walker, Opcode::ToString, integer, { }, call.origin);
    if (!built)
        return walker.hasFailed() || walker.isUnreachable();
    bindForStrings(walker, call.call.id(), built, call.origin);
    return true;
}

static bool hasCountableDigits(Walker& walker, Reference number)
{
    if (!number || !number.isRecord())
        return false;
    Type type = walker.types().at(walker.typeOf(number));
    return type.isInt32() && !type.integers.isEmpty() && type.integers.minimum >= 0;
}

static Reference buildDigitCount(Walker& walker, Reference integer, Origin origin)
{
    Type type = walker.types().at(walker.typeOf(integer));
    int64_t digits = 1;
    Reference length;
    for (int64_t power = 10; power <= type.integers.maximum; power *= 10) {
        if (type.integers.minimum >= power) {
            ++digits;
            continue;
        }
        if (!walker.isInLoop() && (length || type.integers.maximum >= 100))
            return { };
        Reference difference = buildOperatorForStrings(walker, Opcode::Sub, integerForStrings(walker, power - 1), integer, origin);
        Reference hasDigit = difference ? buildOperatorForStrings(walker, Opcode::URShift, difference, integerForStrings(walker, 31), origin) : Reference();
        length = length && hasDigit ? buildOperatorForStrings(walker, Opcode::Add, length, hasDigit, origin) : hasDigit;
        if (!length)
            return { };
    }
    Reference known = integerForStrings(walker, digits);
    return length ? buildOperatorForStrings(walker, Opcode::Add, length, known, origin) : known;
}

Reference buildLengthOfIntegerString(Walker& walker, Reference string, Origin origin)
{
    if (walker.isUnreachable() || !string || !string.isValue() || !isProvenString(walker, walker.typeOf(string)))
        return { };
    Reference integer = integerConvertedToString(walker, string);
    if (!hasCountableDigits(walker, integer))
        return { };
    origin.isTailCall = false;
    return buildDigitCount(walker, integer, origin);
}

Reference buildLengthOnlyJoin(Walker& walker, ValueID text, Origin origin, Vector<ValueID, 2>& joinsThatFailWhereReaderFails)
{
    Vector<Reference, 8> parts;
    if (!walker.isInLoop() || !takePartsOfJoinMadeForOneReader(walker, text, parts, &joinsThatFailWhereReaderFails))
        return { };
    const int64_t longest = Range::length().maximum;
    int64_t lengthOfWrittenTexts = 0;
    Vector<Reference, 2> texts;
    Vector<Reference, mostIntegersMeasuredForOneJoin> integers;
    for (Reference part : parts) {
        if (StringID written = textKnownForStrings(walker, part)) {
            lengthOfWrittenTexts += lengthOfTextForStrings(walker, written);
            continue;
        }
        if (hasCountableDigits(walker, part) && walker.types().at(walker.typeOf(part)).integers.maximum <= greatestIntegerMeasuredForJoin && integers.size() < mostIntegersMeasuredForOneJoin) {
            integers.append(part);
            continue;
        }
        if (texts.size() == maxStringsMeasuredPerJoin || !isProvenString(walker, walker.typeOf(part)))
            return { };
        texts.append(part);
    }
    static constexpr int64_t mostDigits = 5;
    if ((texts.isEmpty() && integers.isEmpty()) || lengthOfWrittenTexts > longest - static_cast<int64_t>(mostIntegersMeasuredForOneJoin) * mostDigits)
        return { };

    Reference length;
    for (Reference part : integers) {
        Reference digits = buildDigitCount(walker, part, origin);
        length = length && digits ? buildOperatorForStrings(walker, Opcode::Add, length, digits, origin) : digits;
        if (!length)
            return { };
    }
    if (texts.isEmpty()) {
        walker.count(StatisticID::JoinsMeasuredByParts);
        return lengthOfWrittenTexts ? buildOperatorForStrings(walker, Opcode::Add, length, integerForStrings(walker, lengthOfWrittenTexts), origin) : length;
    }
    for (Reference part : texts) {
        Reference lengthOfPart = buildLengthOfString(walker, part, origin);
        length = length && lengthOfPart ? buildOperatorForStrings(walker, Opcode::Add, length, lengthOfPart, origin) : lengthOfPart;
        if (!length)
            return { };
    }
    Reference fits = buildOperatorForStrings(walker, Opcode::LessEq, length, integerForStrings(walker, longest - lengthOfWrittenTexts), origin);
    Reference met;
    auto sum = [&](std::span<Reference> results) {
        Reference proven = buildProofBehindTestForStrings(walker, length, fits, OperandIndex::LessEq::left, origin);
        results[0] = proven && lengthOfWrittenTexts ? buildOperatorForStrings(walker, Opcode::Add, proven, integerForStrings(walker, lengthOfWrittenTexts), origin) : proven;
    };
    auto makeJoin = [&](std::span<Reference> results) {
        Draft joined(Opcode::Strcat, origin);
        walker.chooseConservativeFields(joined);
        joined.setPartsOperands(parts.span());
        Reference made = walker.build(WTF::move(joined));
        results[0] = made && !walker.isUnreachable() ? buildLengthOfString(walker, made, origin) : Reference();
    };
    if (!buildArmsForStrings(walker, fits, BlockFrequency::Rare, scopedLambda<void(std::span<Reference>)>(sum), scopedLambda<void(std::span<Reference>)>(makeJoin), singleElementSpan(met)))
        return { };
    walker.count(StatisticID::JoinsMeasuredByParts);
    return met;
}

static bool isCharCodeOfBuiltinCall(const Walker& walker, Reference number)
{
    if (!number.isValue())
        return false;
    const Operation& call = walker.code().at(rootForStrings(walker, number).id());
    if (call.opcode != Opcode::CallBuiltin)
        return false;
    const BuiltinImplementation& implementation = walker.program().builtins().at(call.overload()).code.implementation;
    return implementation.inlineBuiltin == InlineBuiltin::StringCharCodeAt;
}

bool armsBringCharCode(Walker& walker, std::span<const Walker::JoinArm> arms, size_t position)
{
    if (!walker.keptByStrings() || notesOfStrings(walker).codeUnitsBesideCharCodes.isEmpty())
        return false;
    bool someArmHasCodeUnit = false;
    for (const Walker::JoinArm& arm : arms) {
        Reference number = arm.values[position];
        if (!number || !number.isValue() || walker.code().isConstant(number.id()))
            return false;
        bool hasCodeUnit = !!codeUnitOfCharCodeInSight(walker, number);
        if (!hasCodeUnit && !isCharCodeOfBuiltinCall(walker, number))
            return false;
        someArmHasCodeUnit |= hasCodeUnit;
    }
    return someArmHasCodeUnit;
}

void appendCodeUnitOfArm(Walker& walker, Walker::JoinArm& arm, size_t position, Origin origin)
{
    Reference number = arm.values[position];
    Reference codeUnit = codeUnitOfCharCodeInSight(walker, number);
    if (!codeUnit) {
        walker.continueIn(arm.last);
        std::array<Reference, 1> met;
        Reference isInt32 = buildTypeTestForStrings(walker, number, TypeTable::int32, origin);
        bool isBuilt = buildArmsForStrings(walker, isInt32, BlockFrequency::Rare, [&](std::span<Reference> results) {
            Reference integer = buildProofBehindTestForStrings(walker, number, isInt32, OperandIndex::TypeTest::value, origin);
            results[0] = integer ? buildOperatorForStrings(walker, Opcode::BitAnd, integer, integerForStrings(walker, std::numeric_limits<char16_t>::max()), origin) : Reference();
        }, [&](std::span<Reference> results) {
            results[0] = integerForStrings(walker, -1);
        }, met);
        codeUnit = isBuilt ? met[0] : Reference();
        if (!walker.isUnreachable())
            arm.last = walker.currentBlock();
    }
    arm.values.append(codeUnit);
}

void noteCodeUnitBesideCharCode(Walker& walker, Reference charCode, Reference codeUnit)
{
    if (walker.isUnreachable() || !charCode || !charCode.isValue() || !codeUnit || charCode == codeUnit)
        return;
    NotesOfStrings& notes = notesOfStrings(walker);
    uint32_t record = charCode.id().index();
    if (walker.keepsLogsForUndo())
        notes.codeUnitsChanged.append({ record, notes.codeUnitsBesideCharCodes.get(record), false });
    notes.codeUnitsBesideCharCodes.set(record, codeUnit);
}

bool jumpWithBoxedCharCode(Walker& walker, BlockID target, std::span<Reference> arguments, size_t place)
{
    Reference number = arguments[place];
    if (walker.isUnreachable() || !number.isValue() || walker.representationOf(number) != Representation::Double)
        return false;
    Reference codeUnit = codeUnitOfCharCodeInSight(walker, number);
    if (!codeUnit || walker.code().isConstant(number.id()) || walker.code().at(number.id()).block() == walker.currentBlock())
        return false;
    Origin origin = walker.code().at(number.id()).origin();
    origin.isTailCall = false;
    Reference isInside = buildOperatorForStrings(walker, Opcode::LessEq, integerForStrings(walker, 0), codeUnit, origin);
    if (!isInside || walker.isUnreachable())
        return false;
    PrimitiveKnownToFolding decided = primitiveKnownForStrings(walker, isInside);
    if (decided.kind() == PrimitiveKnownToFolding::Kind::Boolean && !decided.asBoolean())
        return false;
    if (decided.kind() != PrimitiveKnownToFolding::Kind::Boolean) {
        BlockID inside = walker.addBlock();
        BlockID outside = walker.addBlock(BlockFrequency::Unlikely);
        walker.branch(isInside, inside, outside);
        walker.continueIn(outside);
        arguments[place] = walker.constant(PrimitiveKnownToFolding::number(PNaN));
        walker.jumpTo(target, arguments);
        walker.continueIn(inside);
        codeUnit = buildProofBehindTestForStrings(walker, codeUnit, isInside, OperandIndex::LessEq::right, origin);
    }
    arguments[place] = codeUnit;
    walker.jumpTo(target, arguments);
    return true;
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
