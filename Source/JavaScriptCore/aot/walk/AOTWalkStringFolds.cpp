/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalkStringFolds.h"

#if ENABLE(AOT)

#include "AOTBuiltinMembers.h"
#include "AOTBuiltinTypes.h"
#include "AOTEffectsDeclared.h"
#include "AOTEffectsOfOperations.h"
#include "AOTFactRecords.h"
#include "AOTIRProgram.h"
#include "AOTStringTable.h"
#include "AOTTypeArena.h"
#include "AOTTypeRelations.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <span>
#include <wtf/text/StringView.h>
#include <wtf/text/WTFString.h>

namespace JSC { namespace AOT { namespace Typed {

using KnownToFolds = PrimitiveKnownToFolding;

static constexpr OpcodeSet opcodesOfStringFolds { Opcode::Strcat, Opcode::ElemGet };
static constexpr OpcodeSet operationsThatMakeTextOfTextsInStringFolds = operationsThatJoinTexts | OpcodeSet { Opcode::ToString };
static constexpr unsigned mostOperationsBetweenTextAndPatternInStringFolds = 4;
static constexpr size_t mostBytesOfKnownPartThatIsNoTextInStringFolds = 32;

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
    const Program& program = walker.program();
    TypeArena& types = walker.types();
    struct Part {
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

    if (end - start == 1) {
        if (!liesInThread(text))
            return KnownToFolds::text(walker.program().strings.codeUnitAt(text, static_cast<uint64_t>(start)));
        char16_t codeUnit = characters[static_cast<unsigned>(start)];
        std::span<const char16_t> piece = singleElementSpan(codeUnit);
        return KnownToFolds::text(types.internText(encodeProgramString(piece).span()));
    }
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
    if (!textFeedsOnlyPatternOfRegExp(walker, join.id()))
        return { };
    return joinKnownTextsForStrings(walker, parts.span(), mostBytesOfPatternMadeByStringFolds);
}

static KnownToFolds answerOnKnownTextInStringFolds(Walker& walker, const BodyValue& value)
{
    static_assert(!OperandIndex::ElemGet::object);
    static_assert(OperandIndex::ElemGet::property == 1);
    Reference subject = walker.referenceOf(value.operand(0));
    StringID text = subject ? textKnownForStrings(walker, subject) : StringID();
    std::optional<int64_t> first = text ? integerKnownInStringFolds(walker, walker.referenceOf(value.operand(1))) : std::nullopt;
    if (!first)
        return { };
    return pieceOfKnownTextForStrings(walker, text, *first, *first + 1);
}

static_assert(opcodesThatFoldOperationOnKnownTextsTakes == (opcodesOfStringFolds));

bool foldOperationOnKnownTextsOfItsOpcodes(Walker& walker, const BodyValue& value)
{
    if (!opcodesOfStringFolds.contains(value.opcode()))
        return false;
    KnownToFolds answer = value.opcode() == Opcode::Strcat ? joinOfKnownPartsInStringFolds(walker, value) : answerOnKnownTextInStringFolds(walker, value);
    Reference constant = answer ? walker.constant(answer) : Reference();
    if (!constant)
        return false;
    walker.bind(value.id(), constant);
    return true;
}

bool isKnownForTerminatorOfFolding(const Walker& walker, Reference operand)
{
    return operand && operand.isRecord() && !!primitiveKnownForStrings(walker, operand);
}

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

bool mayMergeKnownParts(Walker& walker, std::span<const Reference> parts)
{
    bool isLastKnown = false;
    for (Reference part : parts) {
        bool isKnown = part && !!primitiveKnownForStrings(walker, part);
        if (isKnown && (isLastKnown || isEmptyTextInStringFolds(walker, part)))
            return true;
        isLastKnown = isKnown;
    }
    return false;
}

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
                joined = walker.constant(text);
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

std::optional<Effects> effectsOfRowOnOperandsWithoutCode(Walker& walker, BuiltinOverloadID id, TypeID receiver, std::span<const TypeID> arguments)
{

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
    if (effects.mayThrow)
        effects.merge(declaredBoundOf(NamedEffectSet::Allocates));
    return effects;
}

std::optional<Effects> effectsOfRowOnOperandsWithoutCode(Walker& walker, const CallByOneRow& call)
{
    Vector<TypeID, 4> arguments;
    for (Reference argument : call.arguments)
        arguments.append(argument ? walker.typeOf(argument) : TypeID());
    return effectsOfRowOnOperandsWithoutCode(walker, call.overload, call.receiver ? walker.typeOf(call.receiver) : TypeID(), arguments.span());
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

bool bindAnswerOfFoldedCall(Walker& walker, const CallByOneRow& call, KnownToFolds answer)
{
    TypeID byRow = answer ? typeGivenByRowForStrings(walker, call) : TypeID();
    Reference constant = byRow ? walker.constant(answer) : Reference();
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

bool canCallHostHelperForStrings(const Walker& walker, BuiltinID helper)
{
    const BuiltinTable& table = walker.program().builtins();
    if (!table.contains(helper) || table.overloadsOf(helper).empty())
        return false;
    return !hasFeatureGuard(table, helper) || walker.facts().isUsedWithoutNamesFromRunTime(helper);
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

bool foldCallByOneRowOnKnownOperands(Walker& walker, const CallByOneRow& call)
{
    if (!walker.has(Mechanism::Folding) || call.mode != BuiltinCallMode::Call)
        return false;
    const BuiltinTable& table = walker.program().builtins();
    if (!table.at(call.builtin).flags.contains(BuiltinFlag::IsDeterministic))
        return false;
    return foldCallAnsweredByEngine(walker, call);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
