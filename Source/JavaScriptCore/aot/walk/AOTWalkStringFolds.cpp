/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Folding, on texts that the walk knows: a join, a piece, a character, a method of strings whose operands are known primitives is bound to its answer, and nothing is built for it.
// At a suspension and at a throw: nothing. A text never changes, so an answer rests on no state, and only what neither throws nor runs code with the operands at hand is taken away.
// Withheld: the operation goes on down the chain, and the call goes by its row.

#include "config.h"
#include "AOTWalkStringFolds.h"

#if ENABLE(AOT)

#include "AOTBuiltinMembers.h"
#include "AOTBuiltinTypes.h"
#include "AOTEffectsDeclared.h"
#include "AOTEffectsOfOperations.h"
#include "AOTFactRecords.h"
#include "AOTIRProgram.h"
#include "AOTOperandKinds.h"
#include "AOTStringTable.h"
#include "AOTTypeArena.h"
#include "AOTTypeRelations.h"
#include "JSGlobalObjectFunctions.h"
#include "JSString.h"
#include "ParseInt.h"
#include "PureNaN.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <span>
#include <unicode/utf16.h>
#include <wtf/Noncopyable.h>
#include <wtf/text/StringView.h>
#include <wtf/text/WTFString.h>

namespace JSC { namespace AOT { namespace Typed {

using KnownToFolds = PrimitiveKnownToFolding;

static constexpr OpcodeSet opcodesOfStringFolds { Opcode::Strcat, Opcode::ElemGet, Opcode::Substring, Opcode::GetStringCodeUnit };
static constexpr OpcodeSet operationsThatMakeTextOfTextsInStringFolds = operationsThatJoinTexts | OpcodeSet { Opcode::ToString };
static constexpr unsigned mostOperationsBetweenTextAndPatternInStringFolds = 4;
// No number is spelled with more.
static constexpr size_t mostBytesOfKnownPartThatIsNoTextInStringFolds = 32;
// Outside a loop a literal pays against the call while it lists few pieces: a constant and a store each, 16 B, against 40 B for the call. In a loop it saves the search in every turn.
static constexpr unsigned mostPiecesOfSplitWrittenOut = 8;

// ---- Primitives that are known

namespace {

// What ToString gives for a known primitive, in code units. A text that is made here is read and dropped: it never becomes a constant.
class TextOfOperandInStringFolds {
    WTF_MAKE_NONCOPYABLE(TextOfOperandInStringFolds);
public:
    TextOfOperandInStringFolds(const TypeArena& types, KnownToFolds primitive)
    {
        switch (primitive.kind()) {
        case KnownToFolds::Kind::Undefined:
            m_view = "undefined"_s;
            return;
        case KnownToFolds::Kind::Null:
            m_view = "null"_s;
            return;
        case KnownToFolds::Kind::Boolean:
            m_view = primitive.asBoolean() ? "true"_s : "false"_s;
            return;
        case KnownToFolds::Kind::Number:
            m_made = String::number(primitive.asNumber());
            m_view = m_made;
            return;
        case KnownToFolds::Kind::Text:
            m_view = types.viewOf(primitive.asText(), m_buffer);
            return;
        case KnownToFolds::Kind::Unknown:
            break;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    StringView view() const { return m_view; }
    unsigned length() const { return m_view.length(); }

private:
    Vector<char16_t> m_buffer;
    String m_made;
    StringView m_view;
};

// Every operand is a known primitive. An argument that is left out reads as undefined: none of the methods folded here tells the two apart.
struct CallOnKnownOperandsInStringFolds {
    Walker& walker;
    TypeArena& types;
    StringID receiver;
    std::span<const KnownToFolds> arguments;

    KnownToFolds argument(unsigned index) const { return index < arguments.size() ? arguments[index] : KnownToFolds::undefined(); }
    bool isLeftOut(unsigned index) const { return argument(index).kind() == KnownToFolds::Kind::Undefined; }
    double toNumber(unsigned index) const;
    double toIntegerOrInfinity(unsigned index) const
    {
        double converted = toNumber(index);
        return std::isnan(converted) ? 0 : std::trunc(converted) + 0.0;
    }
    KnownToFolds text() const { return KnownToFolds::text(receiver); }
    KnownToFolds emptyText() const { return KnownToFolds::text(walker.program().strings.emptyString()); }
};

struct FoldOfStringMethod {
    InlineBuiltin codeInLine;
    // Unknown: the call throws, or its answer is a text that no operand spells, which the engine makes: foldCallAnsweredByEngine().
    KnownToFolds (*evaluate)(const CallOnKnownOperandsInStringFolds&);
};

} // anonymous namespace

static double toNumberInStringFolds(const TypeArena& types, KnownToFolds primitive)
{
    switch (primitive.kind()) {
    case KnownToFolds::Kind::Undefined:
        return PNaN;
    case KnownToFolds::Kind::Null:
        return 0;
    case KnownToFolds::Kind::Boolean:
        return primitive.asBoolean();
    case KnownToFolds::Kind::Number:
        return primitive.asNumber();
    case KnownToFolds::Kind::Text: {
        Vector<char16_t> buffer;
        return jsToNumber(types.viewOf(primitive.asText(), buffer));
    }
    case KnownToFolds::Kind::Unknown:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

double CallOnKnownOperandsInStringFolds::toNumber(unsigned index) const
{
    return toNumberInStringFolds(types, argument(index));
}

// Null for a number, whose text the table need not hold.
static StringID textInEveryProgramForStringFolds(const StringTable& strings, KnownToFolds primitive)
{
    switch (primitive.kind()) {
    case KnownToFolds::Kind::Undefined:
        return strings.textOfUndefined();
    case KnownToFolds::Kind::Null:
        return strings.textOfNull();
    case KnownToFolds::Kind::Boolean:
        return strings.textOf(primitive.asBoolean());
    case KnownToFolds::Kind::Text:
        return primitive.asText();
    case KnownToFolds::Kind::Number:
        return { };
    case KnownToFolds::Kind::Unknown:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

KnownToFolds joinKnownTextsForStrings(Walker& walker, std::span<const KnownToFolds> parts, size_t mostBytes)
{
    // Nothing here goes through the bytes of a made text: in a row of joins each text is a part of the next, and only the last is read.
    const Program& program = walker.program();
    TypeArena& types = walker.types();
    struct Part {
        // Null: a number.
        StringID text;
        double number { 0 };
    };
    Vector<Part, 8> partsWithBytes;
    size_t numberOfBytes = 0;
    bool hasNarrowPart = false;
    bool hasWidePart = false;
    for (KnownToFolds part : parts) {
        if (!part)
            return { };
        StringID text = textInEveryProgramForStringFolds(program.strings, part);
        size_t numberOfBytesOfPart = text ? types.numberOfBytesOf(text) : String::number(part.asNumber()).length();
        numberOfBytes += numberOfBytesOfPart;
        if (numberOfBytes > mostBytes)
            return { };
        if (!numberOfBytesOfPart)
            continue;
        partsWithBytes.append(Part { text, text ? 0 : part.asNumber() });
        (text && types.isWideText(text) ? hasWidePart : hasNarrowPart) = true;
    }

    if (partsWithBytes.isEmpty())
        return KnownToFolds::text(program.strings.emptyString());
    if (partsWithBytes.size() == 1 && partsWithBytes[0].text)
        return KnownToFolds::text(partsWithBytes[0].text);
    // A piece of such a text that lies in an 8-bit part is an 8-bit string, a piece of a 16-bit literal is not.
    if (hasNarrowPart && hasWidePart)
        return { };
    StringID joined;
    for (const Part& part : partsWithBytes) {
        StringID text = part.text ? part.text : types.internText(byteCast<uint8_t>(String::number(part.number).span8()));
        joined = joined ? types.internJoinedTexts(joined, text) : text;
    }
    return KnownToFolds::text(joined);
}

KnownToFolds pieceOfKnownTextForStrings(Walker& walker, StringID text, int64_t start, int64_t end)
{
    TypeArena& types = walker.types();
    Vector<char16_t> buffer;
    StringView characters = types.viewOf(text, buffer);
    int64_t length = characters.length();
    if (start < 0 || start >= end || end > length)
        return { };
    if (end - start == length)
        return KnownToFolds::text(text);

    // How a string of one code unit is held goes by the code unit alone.
    if (end - start == 1) {
        if (!liesInThread(text))
            return KnownToFolds::text(walker.program().strings.codeUnitAt(text, static_cast<uint64_t>(start)));
        char16_t codeUnit = characters[static_cast<unsigned>(start)];
        std::span<const char16_t> piece = singleElementSpan(codeUnit);
        return KnownToFolds::text(types.internText(encodeProgramString(piece).span()));
    }
    // Such a piece may end in half of a pair, and is held in 16 bits whatever it holds, which a constant is not.
    if (isWideProgramString(types.bytesOf(text)))
        return { };
    StringView piece = characters.substring(static_cast<unsigned>(start), static_cast<unsigned>(end - start));
    return KnownToFolds::text(types.internText(encodeProgramString(piece).span()));
}

static std::optional<int64_t> integerKnownInStringFolds(const Walker& walker, Reference reference)
{
    KnownToFolds known = reference ? primitiveKnownForStrings(walker, reference) : KnownToFolds();
    if (known.kind() != KnownToFolds::Kind::Number)
        return std::nullopt;
    double number = known.asNumber();
    if (!(std::abs(number) <= static_cast<double>(std::numeric_limits<int32_t>::max())) || number != std::trunc(number))
        return std::nullopt;
    return static_cast<int64_t>(number);
}

uint32_t placeInBlockForStrings(const BodyIndex& index, ValueID value)
{
    return value.index() - index.block(index.blockOf(value)).firstValue().index();
}

// ---- new RegExp(a + b)

// The callee is written in front of its arguments, so the walk has been there.
static bool isWrittenAsPatternOfRegExpInStringFolds(Walker& walker, const BodyValue& user, ValueID text)
{
    if (user.opcode() != Opcode::Call && user.opcode() != Opcode::Construct)
        return false;
    std::span<const ValueID> arguments = user.argumentsOperands();
    ValueID writtenCallee = user.calleeOperand();
    if (arguments.empty() || arguments[0] != text || !writtenCallee || !(writtenCallee < text))
        return false;
    Reference callee = walker.referenceOf(writtenCallee);
    TypeID type = callee ? walker.typeOf(callee) : TypeID();
    BuiltinID builtin = type ? singleBuiltinOf(walker.types(), walker.types().at(type)) : BuiltinID();
    const BuiltinTable& table = walker.program().builtins();
    return builtin && table.canonicalID(builtin) == table.canonicalID(Builtins::RegExp);
}

bool textFeedsOnlyPatternOfRegExp(Walker& walker, ValueID text)
{
    const BodyIndex& index = walker.currentBodyIndex();
    for (unsigned step = 0; step <= mostOperationsBetweenTextAndPatternInStringFolds; ++step) {
        ValueID user = index.onlyUsingValue(text);
        if (!user)
            return false;
        BodyValue operation = index.value(user);
        if (isWrittenAsPatternOfRegExpInStringFolds(walker, operation, text))
            return true;
        if (!operationsThatMakeTextOfTextsInStringFolds.contains(operation.opcode()) && !opcodesThatStateTypes.contains(operation.opcode()))
            return false;
        text = user;
    }
    return false;
}

// A RegExp takes the characters of its pattern and nothing else of it.
StringID textOfKnownPatternOfRegExp(Walker& walker, Reference pattern)
{
    StringID emptyText = walker.program().strings.emptyString();
    if (!pattern)
        return emptyText;
    KnownToFolds known = primitiveKnownForStrings(walker, pattern);
    if (known.kind() == KnownToFolds::Kind::Undefined)
        return emptyText;
    KnownToFolds text = known ? joinKnownTextsForStrings(walker, singleElementSpan(known), mostBytesOfPatternMadeByStringFolds) : KnownToFolds();
    return text ? text.asText() : StringID();
}

// ---- The link of the chain

static KnownToFolds joinOfKnownPartsInStringFolds(Walker& walker, const BodyValue& join)
{
    Vector<KnownToFolds, 8> parts;
    for (ValueID written : join.as<Opcode::Strcat>().partsOperands()) {
        Reference part = walker.referenceOf(written);
        KnownToFolds known = part ? primitiveKnownForStrings(walker, part) : KnownToFolds();
        if (!known)
            return { };
        parts.append(known);
    }
    if (KnownToFolds text = joinKnownTextsForStrings(walker, parts.span()))
        return text;
    // A pattern may be as long as a pattern that Image still compiles.
    if (!textFeedsOnlyPatternOfRegExp(walker, join.id()))
        return { };
    return joinKnownTextsForStrings(walker, parts.span(), mostBytesOfPatternMadeByStringFolds);
}

// The characters of a string are its own properties, and nothing can change them. Past the end the read asks String.prototype, so it stays.
static KnownToFolds answerOnKnownTextInStringFolds(Walker& walker, const BodyValue& value)
{
    static_assert(!OperandIndex::ElemGet::object && !OperandIndex::Substring::string && !OperandIndex::GetStringCodeUnit::string);
    static_assert(OperandIndex::ElemGet::property == 1 && OperandIndex::Substring::start == 1 && OperandIndex::GetStringCodeUnit::position == 1 && OperandIndex::Substring::end == 2);
    Reference subject = walker.referenceOf(value.operand(0));
    StringID text = subject ? textKnownForStrings(walker, subject) : StringID();
    std::optional<int64_t> first = text ? integerKnownInStringFolds(walker, walker.referenceOf(value.operand(1))) : std::nullopt;
    if (!first)
        return { };

    switch (value.opcode()) {
    case Opcode::ElemGet:
        return pieceOfKnownTextForStrings(walker, text, *first, *first + 1);
    case Opcode::Substring: {
        std::optional<int64_t> end = integerKnownInStringFolds(walker, walker.referenceOf(value.operand(2)));
        if (!end)
            return { };
        return *first >= *end ? KnownToFolds::text(walker.program().strings.emptyString()) : pieceOfKnownTextForStrings(walker, text, *first, *end);
    }
    case Opcode::GetStringCodeUnit: {
        Vector<char16_t> buffer;
        StringView characters = walker.types().viewOf(text, buffer);
        if (*first < 0 || *first >= static_cast<int64_t>(characters.length()))
            return { };
        return KnownToFolds::integer(characters[static_cast<unsigned>(*first)]);
    }
    default:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static_assert(opcodesThatFoldOperationOnKnownTextsTakes == (opcodesOfStringFolds));

bool foldOperationOnKnownTextsOfItsOpcodes(Walker& walker, const BodyValue& value)
{
    if (!opcodesOfStringFolds.contains(value.opcode()))
        return false;
    KnownToFolds answer = value.opcode() == Opcode::Strcat ? joinOfKnownPartsInStringFolds(walker, value) : answerOnKnownTextInStringFolds(walker, value);
    Reference constant = answer ? constantForStrings(walker, answer) : Reference();
    if (!constant)
        return false;
    walker.bind(value.id(), constant);
    return true;
}

bool isKnownForTerminatorOfFolding(const Walker& walker, Reference operand)
{
    return operand && operand.isRecord() && !!primitiveKnownForStrings(walker, operand);
}

// ---- Rows of joins

// Behind the known parts from `first` on that one text can stand for. How a text is held follows its literal, so a narrow one and a wide one stay apart: `s + "a" + "b" + "中"` joins the first two.
static size_t endOfKnownPartsThatMakeOneTextInStringFolds(Walker& walker, std::span<const Reference> parts, size_t first)
{
    bool isWide = false;
    size_t numberOfBytes = 0;
    size_t end = first;
    for (; end < parts.size(); ++end) {
        KnownToFolds known = parts[end] ? primitiveKnownForStrings(walker, parts[end]) : KnownToFolds();
        if (!known)
            break;
        bool partIsWide = known.isText() && walker.types().isWideText(known.asText());
        numberOfBytes += known.isText() ? walker.types().numberOfBytesOf(known.asText()) : mostBytesOfKnownPartThatIsNoTextInStringFolds;
        if (end > first && (partIsWide != isWide || numberOfBytes > mostBytesOfTextMadeByStringFolds))
            break;
        isWide = partIsWide;
    }
    return end;
}

static bool isEmptyTextInStringFolds(Walker& walker, Reference part)
{
    StringID text = part ? textKnownForStrings(walker, part) : StringID();
    return text && !walker.types().numberOfBytesOf(text);
}

// `a + "x" + 1 + "y"`: known parts that stand side by side are one text.
void mergeKnownPartsOfJoinedText(Walker& walker, Vector<Reference, 8>& parts)
{
    if (!walker.has(Mechanism::Folding) || parts.isEmpty())
        return;
    Reference someEmptyText;
    parts.removeAllMatching([&](Reference part) {
        if (!isEmptyTextInStringFolds(walker, part))
            return false;
        someEmptyText = part;
        return true;
    });
    if (parts.isEmpty()) {
        parts.append(someEmptyText);
        return;
    }
    // `n + ""` is there for the conversion.
    if (parts.size() == 1 && someEmptyText && !isProvenString(walker, walker.typeOf(parts[0]))) {
        parts.append(someEmptyText);
        return;
    }

    size_t numberOfPartsKept = 0;
    for (size_t first = 0; first < parts.size();) {
        size_t end = endOfKnownPartsThatMakeOneTextInStringFolds(walker, parts.span(), first);
        Reference joined;
        if (end - first > 1) {
            Vector<KnownToFolds, 8> known;
            for (Reference part : parts.span().subspan(first, end - first))
                known.append(primitiveKnownForStrings(walker, part));
            if (KnownToFolds text = joinKnownTextsForStrings(walker, known.span()))
                joined = constantForStrings(walker, text);
        }
        if (joined) {
            parts[numberOfPartsKept++] = joined;
            first = end;
            continue;
        }
        end = std::max(end, first + 1);
        while (first < end)
            parts[numberOfPartsKept++] = parts[first++];
    }
    parts.shrink(numberOfPartsKept);
}

// `source.id + "," + target.id`: the read of `target.id` may run code. The link in front of it is joined behind that code, which nobody can tell:
// its parts are primitives, which never change, and together they are too short for the join to be refused.
bool mayStandBetweenLinksOfJoinedText(Walker& walker, const BodyValue& between, std::span<const Reference> partsSoFar)
{
    if (maySuspendAsWritten(between))
        return false;
    uint64_t longest = 0;
    for (Reference part : partsSoFar) {
        if (!part)
            return false;
        if (StringID text = textKnownForStrings(walker, part)) {
            longest += walker.types().numberOfBytesOf(text);
            continue;
        }
        Type type = walker.types().at(walker.typeOf(part));
        if (!hasTextByItself(type))
            return false;
        longest += longestTextOf(walker.types(), type);
    }
    return longest <= JSString::MaxLength;
}

// As findJoinThatAloneTakesInWalkTexts() of AOTWalkTypedTexts.cpp, which is asked first.
ValueID findJoinThatTakesLinkPastCode(Walker& walker, ValueID text, std::span<const Reference> partsSoFar)
{
    const BodyIndex& index = walker.currentBodyIndex();
    BodyBlock block = index.block(index.blockOf(text));
    ValueID user = index.onlyUsingValue(text);
    if (!user || user.index() >= block.firstValue().index() + walker.endOfStretchBeingWalked() || !operationsThatJoinTexts.contains(index.opcodeOf(user)))
        return { };
    for (uint32_t between = text.index() + 1; between < user.index(); ++between) {
        Opcode opcode = index.opcodeOf(ValueID(between));
        if (opcodesThatStateTypes.contains(opcode) || declaredBoundInTable(opcode).isUnobservable())
            continue;
        if (!mayStandBetweenLinksOfJoinedText(walker, index.value(ValueID(between)), partsSoFar))
            return { };
    }
    return user;
}

// ---- What a row does on operands that run no code

std::optional<Effects> effectsOfRowOnOperandsWithoutCode(Walker& walker, BuiltinOverloadID id, TypeID receiver, std::span<const TypeID> arguments, std::span<const BuiltinOperand> objectsThatRunNoCode)
{
    static constexpr OperandUses usesThatChangeOperand {
        OperandUse::WritesContents, OperandUse::MovesElements, OperandUse::StoresByKey, OperandUse::DeletesByKey, OperandUse::AssignsFrom,
        OperandUse::DefinesName, OperandUse::TakesDescriptor, OperandUse::ChangesIntegrity, OperandUse::ChangesPrototype,
    };

    const BuiltinTable& table = walker.program().builtins();
    TypeArena& types = walker.types();
    const BuiltinOverload& row = table.at(id);
    if (row.isUndeclared() || row.libraryFunction())
        return std::nullopt;
    if (row.effects.capability == BuiltinCapability::JS && !row.flags.contains(BuiltinOverloadFlag::RunsOnlyOperandCode))
        return std::nullopt;
    if (table.at(row.builtin).flags.containsAny({ BuiltinFlag::EvaluatesText, BuiltinFlag::LoadsModuleByName, BuiltinFlag::RunsUnseenCode }))
        return std::nullopt;
    if (table.has(BuiltinMark::RunsWaitingJobs, row.builtin) || table.has(BuiltinMark::MayTerminateProcess, row.builtin))
        return std::nullopt;
    for (const BuiltinHandlerCall& call : row.flow.handlerCalls) {
        if (!isCalledAfterReturn(call.times))
            return std::nullopt;
    }
    // A row names nothing only where it has no operand either, and what such a function answers comes out of some state.
    if (!row.receiver && row.parameters.empty() && row.effects.reads.isEmpty() && row.effects.writes.isEmpty())
        return std::nullopt;

    OperandUses uses;
    if (row.receiver)
        uses.add(row.receiver->uses);
    for (const BuiltinParameter& parameter : row.parameters)
        uses.add(parameter.uses);
    if ((uses.containsAny(usesThatChangeOperand) && row.effects.writes.isEmpty()) || (uses.contains(OperandUse::ReadsContents) && row.effects.reads.isEmpty()))
        return std::nullopt;

    Effects effects = Effects::none();
    effects.reads.add(row.effects.reads);
    effects.writes.add(row.effects.writes);
    if (row.effects.capability != BuiltinCapability::None) {
        Effects ofHeap = declaredBoundOf(NamedEffectSet::Allocates);
        ofHeap.merge(declaredBoundOf(NamedEffectSet::UsesRuntime));
        ofHeap.merge(declaredBoundOf(NamedEffectSet::ResolvesRopes));
        ofHeap.mayThrow = ThrowEffect::onlyForLackOfMemory();
        effects.merge(ofHeap);
    }
    if (row.throws())
        effects.mayThrow = true;

    TypingContext typing { walker.program(), types, PositionTypes::knowingNothing() };
    auto addOperand = [&](BuiltinOperand position, TypeID operand) {
        // An argument behind the last parameter is evaluated already, and the row reads none.
        const BuiltinParameter* parameter = table.parameterFor(row, position);
        if (!parameter)
            return true;
        if (!operand)
            return false;
        Type type = types.at(operand);
        if (readsCharactersOfString(parameter->conversion) && (type.primitives & Primitive::AnyString)) {
            Effects ofReadingCharacters = declaredBoundOf(NamedEffectSet::ResolvesRopes);
            ofReadingCharacters.mayThrow = ThrowEffect::onlyForLackOfMemory();
            effects.merge(ofReadingCharacters);
        }
        if (std::ranges::find(objectsThatRunNoCode, position) != objectsThatRunNoCode.end())
            return true;
        bool mayBeObject = type.hasObjects() || !type.isJavaScriptValue();
        if (mayBeObject && !(parameter->protocols.isEmpty() && parameter->uses.containsOnly({ OperandUse::Inspects })))
            return false;
        if (!parameter->protocols.isEmpty())
            effects.merge(effectsOfProtocolsOn(typing, parameter->protocols, operand));
        return !effects.mayRunOtherCode();
    };
    if (row.receiver && !addOperand(BuiltinOperand::receiver(), receiver))
        return std::nullopt;
    for (unsigned index = 0; index < arguments.size(); ++index) {
        if (!addOperand(BuiltinOperand::argument(index), arguments[index]))
            return std::nullopt;
    }
    // The error is allocated on the way to the handler.
    if (effects.mayThrow)
        effects.merge(declaredBoundOf(NamedEffectSet::Allocates));
    return effects;
}

std::optional<Effects> effectsOfRowOnOperandsWithoutCode(Walker& walker, const CallByOneRow& call, std::span<const BuiltinOperand> objectsThatRunNoCode)
{
    Vector<TypeID, 4> arguments;
    for (Reference argument : call.arguments)
        arguments.append(argument ? walker.typeOf(argument) : TypeID());
    return effectsOfRowOnOperandsWithoutCode(walker, call.overload, call.receiver ? walker.typeOf(call.receiver) : TypeID(), arguments.span(), objectsThatRunNoCode);
}

TypeID typeGivenByRowForStrings(Walker& walker, const CallByOneRow& call)
{
    Draft byRow(Opcode::CallBuiltin, call.origin);
    walker.chooseConservativeFields(byRow);
    byRow.setBuiltinCallForm(BuiltinCallForm::Whole);
    byRow.setOverload(call.overload);
    if (call.receiver)
        byRow.setReceiverOperand(call.receiver);
    byRow.setArgumentsOperands(call.arguments);
    return walker.resultTypeOf(byRow);
}

// Where the row and the fold disagree, the function itself decides at run time.
bool bindAnswerOfFoldedCall(Walker& walker, const CallByOneRow& call, KnownToFolds answer)
{
    TypeID byRow = answer ? typeGivenByRowForStrings(walker, call) : TypeID();
    Reference constant = byRow ? constantForStrings(walker, answer) : Reference();
    if (!constant || !walker.types().isSubset(walker.typeOf(constant), byRow))
        return false;
    walker.bind(call.call.id(), constant);
    return true;
}

Reference buildProofOfTypeTestForStrings(Walker& walker, Reference value, Reference test, Origin origin)
{
    Draft proof(Opcode::Narrowed, origin);
    walker.chooseConservativeFields(proof);
    proof.setValueOperand(value);
    proof.setTestOperand(test);
    proof.setOperandIndex(OperandIndex::TypeTest::value);
    proof.setIsOnTakenEdge(true);
    return walker.build(WTF::move(proof));
}

// A feature that no listed use of a built-in asks for is left out of the executable, and a function that throws stands in the place of its entries.
bool canCallHostHelperForStrings(const Walker& walker, BuiltinID helper)
{
    const BuiltinTable& table = walker.program().builtins();
    if (!table.contains(helper) || table.overloadsOf(helper).empty())
        return false;
    return !hasFeatureGuard(table, helper) || findFactRow(walker.facts().builtinUses(), helper.index(), &BuiltinUseRecord::builtin);
}

Reference buildCallOfHostHelperForStrings(Walker& walker, BuiltinID helper, std::span<const Reference> arguments, Origin origin)
{
    BuiltinOverloadID row = *walker.program().builtins().overloadIDsOf(helper).begin();
    Vector<TypeID, 4> types;
    for (Reference argument : arguments)
        types.append(walker.typeOf(argument));
    std::optional<Effects> effects = effectsOfRowOnOperandsWithoutCode(walker, row, { }, types.span());

    Draft call(Opcode::CallBuiltin, origin);
    walker.chooseConservativeFields(call);
    call.setBuiltinCallForm(BuiltinCallForm::Whole);
    call.setOverload(row);
    call.setArgumentsOperands(arguments);
    call.standsForNoValueOfInput = true;
    return walker.build(WTF::move(call), effects ? &*effects : nullptr);
}

// A throw is not counted here: each fold gives an answer only where the built-in does not throw.
static bool hasNoVisibleEffectsInStringFolds(Walker& walker, const CallByOneRow& call)
{
    std::optional<Effects> effects = effectsOfRowOnOperandsWithoutCode(walker, call);
    return effects && effects->alwaysCompletes() && effects->writes.containsOnly(heapsWhoseWritesNobodySees);
}

// ---- Methods of strings, by code units

static unsigned clampPositionInStringFolds(double position, unsigned length)
{
    if (!(position > 0))
        return 0;
    return position < length ? static_cast<unsigned>(position) : length;
}

static unsigned relativePositionInStringFolds(double position, unsigned length)
{
    return clampPositionInStringFolds(position < 0 ? length + position : position, length);
}

static unsigned lengthOfReceiverInStringFolds(const CallOnKnownOperandsInStringFolds& call)
{
    return TextOfOperandInStringFolds(call.types, call.text()).length();
}

// A row answers with a text only where that is the receiver's own, a piece of it that pieceOfKnownTextForStrings() cuts, or the empty one, which the table of strings holds whatever the program spells.
// How any other text is held at run time follows from how the engine makes it, so the engine is asked for it.
static KnownToFolds partOfReceiverInStringFolds(const CallOnKnownOperandsInStringFolds& call, unsigned length, unsigned start, unsigned end)
{
    if (start >= end)
        return call.emptyText();
    return !start && end == length ? call.text() : pieceOfKnownTextForStrings(call.walker, call.receiver, start, end);
}

static KnownToFolds foldedCharCodeAt(const CallOnKnownOperandsInStringFolds& call)
{
    TextOfOperandInStringFolds text(call.types, call.text());
    double position = call.toIntegerOrInfinity(0);
    if (position < 0 || position >= text.length())
        return KnownToFolds::number(PNaN);
    return KnownToFolds::integer(text.view()[static_cast<unsigned>(position)]);
}

static KnownToFolds foldedCodePointAt(const CallOnKnownOperandsInStringFolds& call)
{
    TextOfOperandInStringFolds text(call.types, call.text());
    StringView view = text.view();
    double position = call.toIntegerOrInfinity(0);
    if (position < 0 || position >= view.length())
        return KnownToFolds::undefined();
    unsigned index = static_cast<unsigned>(position);
    char16_t first = view[index];
    if (U16_IS_LEAD(first) && index + 1 < view.length() && U16_IS_TRAIL(view[index + 1]))
        return KnownToFolds::integer(U16_GET_SUPPLEMENTARY(first, view[index + 1]));
    return KnownToFolds::integer(first);
}

static KnownToFolds foldedCharAt(const CallOnKnownOperandsInStringFolds& call)
{
    unsigned length = lengthOfReceiverInStringFolds(call);
    double position = call.toIntegerOrInfinity(0);
    if (position < 0 || position >= length)
        return call.emptyText();
    return pieceOfKnownTextForStrings(call.walker, call.receiver, static_cast<int64_t>(position), static_cast<int64_t>(position) + 1);
}

static KnownToFolds foldedAt(const CallOnKnownOperandsInStringFolds& call)
{
    unsigned length = lengthOfReceiverInStringFolds(call);
    double position = call.toIntegerOrInfinity(0);
    if (position < 0)
        position += length;
    if (position < 0 || position >= length)
        return KnownToFolds::undefined();
    return pieceOfKnownTextForStrings(call.walker, call.receiver, static_cast<int64_t>(position), static_cast<int64_t>(position) + 1);
}

static int64_t findInStringFolds(StringView text, StringView searched, unsigned start)
{
    if (searched.isEmpty())
        return start;
    size_t found = text.find(searched, start);
    return found == notFound ? -1 : static_cast<int64_t>(found);
}

static KnownToFolds foldedIndexOf(const CallOnKnownOperandsInStringFolds& call)
{
    TextOfOperandInStringFolds text(call.types, call.text());
    TextOfOperandInStringFolds searched(call.types, call.argument(0));
    return KnownToFolds::integer(findInStringFolds(text.view(), searched.view(), clampPositionInStringFolds(call.toIntegerOrInfinity(1), text.length())));
}

static KnownToFolds foldedIncludes(const CallOnKnownOperandsInStringFolds& call)
{
    TextOfOperandInStringFolds text(call.types, call.text());
    TextOfOperandInStringFolds searched(call.types, call.argument(0));
    return KnownToFolds::boolean(findInStringFolds(text.view(), searched.view(), clampPositionInStringFolds(call.toIntegerOrInfinity(1), text.length())) >= 0);
}

static KnownToFolds foldedLastIndexOf(const CallOnKnownOperandsInStringFolds& call)
{
    TextOfOperandInStringFolds text(call.types, call.text());
    TextOfOperandInStringFolds searched(call.types, call.argument(0));
    if (searched.length() > text.length())
        return KnownToFolds::integer(-1);
    double position = call.toNumber(1);
    unsigned start = clampPositionInStringFolds(std::isnan(position) ? std::numeric_limits<double>::infinity() : std::trunc(position), text.length() - searched.length());
    if (searched.view().isEmpty())
        return KnownToFolds::integer(start);
    size_t found = text.view().reverseFind(searched.view(), start);
    return KnownToFolds::integer(found == notFound ? -1 : static_cast<int64_t>(found));
}

static KnownToFolds foldedStartsWith(const CallOnKnownOperandsInStringFolds& call)
{
    TextOfOperandInStringFolds text(call.types, call.text());
    TextOfOperandInStringFolds searched(call.types, call.argument(0));
    return KnownToFolds::boolean(text.view().hasInfixStartingAt(searched.view(), clampPositionInStringFolds(call.toIntegerOrInfinity(1), text.length())));
}

static KnownToFolds foldedEndsWith(const CallOnKnownOperandsInStringFolds& call)
{
    TextOfOperandInStringFolds text(call.types, call.text());
    TextOfOperandInStringFolds searched(call.types, call.argument(0));
    unsigned end = call.isLeftOut(1) ? text.length() : clampPositionInStringFolds(call.toIntegerOrInfinity(1), text.length());
    return KnownToFolds::boolean(text.view().hasInfixEndingAt(searched.view(), end));
}

static bool isWellFormedInStringFolds(StringView text)
{
    if (text.is8Bit())
        return true;
    for (unsigned index = 0; index < text.length(); ++index) {
        if (!U16_IS_SURROGATE(text[index]))
            continue;
        if (!U16_IS_SURROGATE_LEAD(text[index]) || index + 1 == text.length() || !U16_IS_TRAIL(text[index + 1]))
            return false;
        ++index;
    }
    return true;
}

static KnownToFolds foldedIsWellFormed(const CallOnKnownOperandsInStringFolds& call)
{
    return KnownToFolds::boolean(isWellFormedInStringFolds(TextOfOperandInStringFolds(call.types, call.text()).view()));
}

static KnownToFolds foldedToWellFormed(const CallOnKnownOperandsInStringFolds& call)
{
    return isWellFormedInStringFolds(TextOfOperandInStringFolds(call.types, call.text()).view()) ? call.text() : KnownToFolds();
}

static KnownToFolds foldedSlice(const CallOnKnownOperandsInStringFolds& call)
{
    unsigned length = lengthOfReceiverInStringFolds(call);
    unsigned start = relativePositionInStringFolds(call.toIntegerOrInfinity(0), length);
    unsigned end = call.isLeftOut(1) ? length : relativePositionInStringFolds(call.toIntegerOrInfinity(1), length);
    return partOfReceiverInStringFolds(call, length, start, end);
}

static KnownToFolds foldedSubstring(const CallOnKnownOperandsInStringFolds& call)
{
    unsigned length = lengthOfReceiverInStringFolds(call);
    unsigned first = clampPositionInStringFolds(call.toIntegerOrInfinity(0), length);
    unsigned second = call.isLeftOut(1) ? length : clampPositionInStringFolds(call.toIntegerOrInfinity(1), length);
    return partOfReceiverInStringFolds(call, length, std::min(first, second), std::max(first, second));
}

static KnownToFolds foldedSubstr(const CallOnKnownOperandsInStringFolds& call)
{
    unsigned length = lengthOfReceiverInStringFolds(call);
    unsigned start = relativePositionInStringFolds(call.toIntegerOrInfinity(0), length);
    unsigned count = call.isLeftOut(1) ? length : clampPositionInStringFolds(call.toIntegerOrInfinity(1), length);
    return partOfReceiverInStringFolds(call, length, start, std::min(start + count, length));
}

template<bool trimsStart, bool trimsEnd>
static KnownToFolds foldedTrim(const CallOnKnownOperandsInStringFolds& call)
{
    TextOfOperandInStringFolds text(call.types, call.text());
    StringView view = text.view();
    unsigned start = 0;
    unsigned end = view.length();
    while (trimsStart && start < end && isStrWhiteSpace(view[start]))
        ++start;
    while (trimsEnd && end > start && isStrWhiteSpace(view[end - 1]))
        --end;
    return partOfReceiverInStringFolds(call, view.length(), start, end);
}

template<bool toUpperCase>
static KnownToFolds foldedCaseConversion(const CallOnKnownOperandsInStringFolds& call)
{
    String text = TextOfOperandInStringFolds(call.types, call.text()).view().toString();
    // Beyond ASCII it is ICU that says whether a character has another case, and on Darwin the program runs with the ICU of its machine.
    if (!text.containsOnlyASCII())
        return { };
    String converted = toUpperCase ? text.convertToUppercaseWithoutLocale() : text.convertToLowercaseWithoutLocale();
    return converted == text ? call.text() : KnownToFolds();
}

static KnownToFolds foldedRepeat(const CallOnKnownOperandsInStringFolds& call)
{
    double count = call.toIntegerOrInfinity(0);
    if (count < 0 || std::isinf(count))
        return { };
    if (!count || !lengthOfReceiverInStringFolds(call))
        return call.emptyText();
    return count == 1 ? call.text() : KnownToFolds();
}

static KnownToFolds foldedPadding(const CallOnKnownOperandsInStringFolds& call)
{
    if (call.toIntegerOrInfinity(0) <= lengthOfReceiverInStringFolds(call))
        return call.text();
    KnownToFolds filler = call.argument(1);
    bool fillsWithNothing = filler.isText() && !TextOfOperandInStringFolds(call.types, filler).length();
    return fillsWithNothing ? call.text() : KnownToFolds();
}

static constexpr FoldOfStringMethod foldsOfStringMethods[] = {
    { InlineBuiltin::StringCharCodeAt, foldedCharCodeAt },
    { InlineBuiltin::StringCodePointAt, foldedCodePointAt },
    { InlineBuiltin::StringCharAt, foldedCharAt },
    { InlineBuiltin::StringAt, foldedAt },
    { InlineBuiltin::StringIndexOf, foldedIndexOf },
    { InlineBuiltin::StringLastIndexOf, foldedLastIndexOf },
    { InlineBuiltin::StringIncludes, foldedIncludes },
    { InlineBuiltin::StringStartsWith, foldedStartsWith },
    { InlineBuiltin::StringEndsWith, foldedEndsWith },
    { InlineBuiltin::StringIsWellFormed, foldedIsWellFormed },
    { InlineBuiltin::StringToWellFormed, foldedToWellFormed },
    { InlineBuiltin::StringSlice, foldedSlice },
    { InlineBuiltin::StringSubstring, foldedSubstring },
    { InlineBuiltin::StringSubstr, foldedSubstr },
    { InlineBuiltin::StringTrim, foldedTrim<true, true> },
    { InlineBuiltin::StringTrimStart, foldedTrim<true, false> },
    { InlineBuiltin::StringTrimEnd, foldedTrim<false, true> },
    { InlineBuiltin::StringToLowerCase, foldedCaseConversion<false> },
    { InlineBuiltin::StringToUpperCase, foldedCaseConversion<true> },
    { InlineBuiltin::StringRepeat, foldedRepeat },
    { InlineBuiltin::StringPadStart, foldedPadding },
    { InlineBuiltin::StringPadEnd, foldedPadding },
};

static KnownToFolds evaluateOnKnownOperandsInStringFolds(Walker& walker, const CallByOneRow& call)
{
    const FoldOfStringMethod* fold = nullptr;
    for (const FoldOfStringMethod& candidate : foldsOfStringMethods) {
        if (call.codeInLine == candidate.codeInLine)
            fold = &candidate;
    }
    StringID receiver = fold && call.receiver ? textKnownForStrings(walker, call.receiver) : StringID();
    if (!receiver)
        return { };
    Vector<KnownToFolds, 4> arguments;
    for (Reference argument : call.arguments) {
        KnownToFolds known = argument ? primitiveKnownForStrings(walker, argument) : KnownToFolds();
        if (!known)
            return { };
        arguments.append(known);
    }
    return fold->evaluate({ walker, walker.types(), receiver, arguments.span() });
}

// ---- End tests and searches that are decided early

static Range lengthsOfStringByTypeInStringFolds(Walker& walker, Reference string)
{
    Draft lengthOfString(Opcode::GetStringLength);
    lengthOfString.setStringOperand(string);
    TypeID byRule = walker.resultTypeOf(lengthOfString);
    Type length = byRule ? walker.types().at(byRule) : Type();
    return byRule && length.isInteger() ? length.integers : Range::length();
}

static bool answersWhetherTextIsThereInStringFolds(InlineBuiltin method)
{
    return method == InlineBuiltin::StringStartsWith || method == InlineBuiltin::StringEndsWith || method == InlineBuiltin::StringIncludes;
}

// A subject that is shorter than the text holds it nowhere, wherever the search begins.
static KnownToFolds answerForSubjectTooShortInStringFolds(Walker& walker, const CallByOneRow& call)
{
    bool answersPlace = call.codeInLine == InlineBuiltin::StringIndexOf || call.codeInLine == InlineBuiltin::StringLastIndexOf;
    if (!answersPlace && !answersWhetherTextIsThereInStringFolds(*call.codeInLine))
        return { };
    if (call.arguments.empty() || !call.arguments[0] || !isProvenString(walker, walker.typeOf(call.receiver)))
        return { };
    StringID sought = textKnownForStrings(walker, call.arguments[0]);
    if (!sought)
        return { };
    Vector<char16_t> buffer;
    int64_t lengthOfSought = walker.types().viewOf(sought, buffer).length();
    if (lengthsOfStringByTypeInStringFolds(walker, call.receiver).maximum >= lengthOfSought)
        return { };
    return answersPlace ? KnownToFolds::integer(-1) : KnownToFolds::boolean(false);
}

// `if (s.startsWith("--")) { .. s.startsWith("--") .. }`: the one way into the block leads over a Branch on the same question of the same string.
static KnownToFolds answerOfSameQuestionThatLedHereInStringFolds(Walker& walker, const CallByOneRow& call)
{
    if (call.call.opcode() != Opcode::Call || !answersWhetherTextIsThereInStringFolds(*call.codeInLine))
        return { };
    std::optional<TestThatLedHere> led = testThatLedToBlockOf(walker, call.call);
    const BodyIndex& index = walker.currentBodyIndex();
    if (!led || index.isParameter(led->test) || index.opcodeOf(led->test) != Opcode::Call)
        return { };

    BodyValue earlier = index.value(led->test);
    Reference callee = walker.referenceOf(earlier.calleeOperand());
    TypeID typeOfCallee = callee ? walker.typeOf(callee) : TypeID();
    BuiltinID asked = typeOfCallee ? singleBuiltinOf(walker.types(), walker.types().at(typeOfCallee)) : BuiltinID();
    if (!asked || walker.program().builtins().canonicalID(asked) != call.builtin)
        return { };

    auto areOneValue = [&](ValueID first, ValueID second) {
        Reference one = first ? walker.referenceOf(first) : Reference();
        Reference other = second ? walker.referenceOf(second) : Reference();
        return one && other && one.isRecord() && other.isRecord() && rootForStrings(walker, one) == rootForStrings(walker, other);
    };
    std::span<const ValueID> argumentsThen = earlier.argumentsOperands();
    std::span<const ValueID> argumentsNow = call.call.argumentsOperands();
    if (argumentsThen.size() != argumentsNow.size() || !areOneValue(earlier.thisValueOperand(), call.call.thisValueOperand()))
        return { };
    for (size_t position = 0; position < argumentsNow.size(); ++position) {
        if (!areOneValue(argumentsThen[position], argumentsNow[position]))
            return { };
    }
    return KnownToFolds::boolean(led->holds);
}

// ---- split

// `"red,green".split(",")` where the walk has brought the two texts together. Whoever gets the list gets what the function makes: nothing is asked of its readers.
static bool buildListOfPiecesOfKnownTextInStringFolds(Walker& walker, const CallByOneRow& call)
{
    TypeArena& types = walker.types();
    if (call.codeInLine != InlineBuiltin::StringSplit || !call.receiver || call.arguments.empty() || call.arguments.size() > 2)
        return false;
    if (call.arguments.size() == 2 && primitiveKnownForStrings(walker, call.arguments[1]).kind() != KnownToFolds::Kind::Undefined)
        return false;
    StringID knownText = textKnownForStrings(walker, call.receiver);
    StringID knownSeparator = textKnownForStrings(walker, call.arguments[0]);
    if (!knownText || !knownSeparator || types.bytesOf(knownText).size() > mostBytesOfTextMadeByStringFolds)
        return false;
    Vector<char16_t> bufferOfText;
    Vector<char16_t> bufferOfSeparator;
    StringView text = types.viewOf(knownText, bufferOfText);
    StringView separator = types.viewOf(knownSeparator, bufferOfSeparator);
    // Whatever is said of the result of the function holds of a literal that holds the strings which the function would have put there, except for the kind of storage.
    TypeID list = typeGivenByRowForStrings(walker, call);
    if (separator.isEmpty() || !list || !hasKind(types, list, OperandKind::Array) || walker.facts().someObjectHas(types, list, ObjectFact::HasDecidedStorage))
        return false;
    if (!hasNoVisibleEffectsInStringFolds(walker, call))
        return false;

    // Where each piece begins and how long it is.
    unsigned mostPieces = walker.isInLoop() ? maxComputedElementsOfArrayLiteral : mostPiecesOfSplitWrittenOut;
    Vector<std::pair<unsigned, unsigned>, maxComputedElementsOfArrayLiteral> bounds;
    unsigned start = 0;
    for (size_t found = text.find(separator, start); found != notFound; found = text.find(separator, start)) {
        if (bounds.size() + 1 == mostPieces)
            return false;
        bounds.append({ start, static_cast<unsigned>(found) - start });
        start = static_cast<unsigned>(found) + separator.length();
    }
    bounds.append({ start, text.length() - start });

    Vector<Reference, maxComputedElementsOfArrayLiteral> pieces;
    for (auto [startOfPiece, lengthOfPiece] : bounds) {
        StringID piece = knownText;
        if (!lengthOfPiece)
            piece = walker.program().strings.emptyString();
        else if (lengthOfPiece != text.length())
            piece = types.internText(encodeProgramString(text.substring(startOfPiece, lengthOfPiece)).span());
        pieces.append(constantForStrings(walker, KnownToFolds::text(piece)));
    }

    Draft literal(Opcode::NewArray, call.origin);
    walker.chooseConservativeFields(literal);
    literal.setContents(TypeTable::anything);
    literal.setElementsOperands(pieces.span());
    literal.expected = call.expected;
    literal.inputValueGiven = call.call.id();
    Reference built = walker.build(WTF::move(literal));
    if (!built)
        return true;
    noteFreshList(walker, built);
    walker.bind(call.call.id(), built);
    return true;
}

// ---- The library's test for a class of the host

static bool canBeJudgedByTypeInStringFolds(const Type& type)
{
    return type.isJavaScriptValue() && !type.isNever() && !(type.primitives & (Primitive::Absent | Primitive::Unspecified));
}

// A closure belongs to no class that a built-in tests for. Nothing here knows the class of a prototype object, a dictionary or a host object.
static bool namesObjectOfUnknownClassInStringFolds(const TypeArena& types, const Type& type)
{
    if (type.traitsOfNamed.contains(ObjectTrait::IsBuiltin))
        return true;
    if (!type.traitsOfNamed.contains(ObjectTrait::HasNoShape))
        return false;
    bool namesOne = false;
    types.at(type.objects).forEach([&](ObjectID object) {
        const ObjectRecord& record = types.at(object);
        namesOne |= record.has(ObjectTrait::HasNoShape) && !record.has(ObjectTrait::IsClosure);
    });
    return namesOne;
}

// `$inherits(HostClass, value)` on a primitive, an object of the program or of one of the engine's classes.
static KnownToFolds answerOfHostClassTestInStringFolds(Walker& walker, const CallByOneRow& call)
{
    if (call.arguments.size() != 2 || !call.arguments[1])
        return { };
    TypeArena& types = walker.types();
    const BuiltinTable& table = walker.program().builtins();
    Type type = types.at(walker.typeOf(call.arguments[1]));
    if (!canBeJudgedByTypeInStringFolds(type) || type.hasUnboundedUnknown() || namesObjectOfUnknownClassInStringFolds(types, type))
        return { };
    for (LayoutID layout : membersOf(types, type)) {
        if (builtinClassWrittenAbove(types, layout))
            return { };
    }
    for (const ClassPart& part : classPartsOf(types, type)) {
        if (table.at(part.builtinClass).origin != BuiltinOrigin::Engine)
            return { };
    }
    return KnownToFolds::boolean(false);
}

// ---- The door of BuiltinCalls

bool foldCallByOneRowOnKnownOperands(Walker& walker, const CallByOneRow& call)
{
    if (!walker.has(Mechanism::Folding) || call.mode != BuiltinCallMode::Call)
        return false;
    const BuiltinTable& table = walker.program().builtins();
    if (table.contains(Builtins::PrivateGlobalsInherits) && call.builtin == table.canonicalID(Builtins::PrivateGlobalsInherits)) {
        KnownToFolds answer = answerOfHostClassTestInStringFolds(walker, call);
        return answer && hasNoVisibleEffectsInStringFolds(walker, call) && bindAnswerOfFoldedCall(walker, call, answer);
    }
    // The pieces of a text at a text depend on the two alone. The table says nothing of split as a whole, which has rows that match.
    if (buildListOfPiecesOfKnownTextInStringFolds(walker, call))
        return true;
    // The table's word that the answer depends on the operands and on nothing else: no clock, no locale, no state.
    if (!table.at(call.builtin).flags.contains(BuiltinFlag::IsDeterministic))
        return false;

    if (call.codeInLine && call.receiver) {
        KnownToFolds answer = evaluateOnKnownOperandsInStringFolds(walker, call);
        if (!answer)
            answer = answerForSubjectTooShortInStringFolds(walker, call);
        if (!answer)
            answer = answerOfSameQuestionThatLedHereInStringFolds(walker, call);
        if (answer)
            return hasNoVisibleEffectsInStringFolds(walker, call) && bindAnswerOfFoldedCall(walker, call, answer);
    }
    return foldCallAnsweredByEngine(walker, call);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
