/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTBuiltinTypes.h"
#include "AOTEffectsDeclared.h"
#include "AOTEffectsOfOperations.h"
#include "AOTIRProgram.h"
#include "AOTOpcodeSets.h"
#include "AOTTypeArena.h"
#include "AOTWalkTextBuilderRows.h"
#include "AOTWalkTextBuilders.h"
#include "AOTWalkStringFolds.h"
#include "AOTWalkStrings.h"
#include "AOTWalkKnown.h"
#include <algorithm>
#include <bit>
#include <optional>
#include <span>

namespace JSC { namespace AOT { namespace Typed {

using PartsOfJoinedText = Vector<Reference, 8>;
using JoinsTakenApart = Vector<ValueID, 2>;

static constexpr size_t mostPartsOfJoinedText = 32;

static constexpr OpcodeSet accessesUnderKeysInWalkTexts { Opcode::ElemGet, Opcode::ElemSet, Opcode::ElemHas, Opcode::ElemDelete, Opcode::ElemDefine };
static_assert(OperandIndex::ElemGet::property == OperandIndex::ElemSet::property && OperandIndex::ElemGet::property == OperandIndex::ElemHas::property);
static_assert(OperandIndex::ElemGet::property == OperandIndex::ElemDelete::property && OperandIndex::ElemGet::property == OperandIndex::ElemDefine::property);

namespace {

using UseOfTextAsKey = Walker::UseOfTextAsKey;

} // anonymous namespace

static bool takesKeyOfCollectionInWalkTexts(Walker& walker, const BodyValue& user, ValueID text)
{
    if (user.opcode() != Opcode::Call)
        return false;
    auto call = user.as<Opcode::Call>();
    auto arguments = call.argumentsOperands();
    ValueID receiver = call.thisValueOperand();
    if (!arguments.size() || *arguments.begin() != text || !receiver)
        return false;

    TypeArena& types = walker.types();
    TypeID collections = types.unionOf(types.builtinClass(BuiltinClasses::Map), types.builtinClass(BuiltinClasses::Set));
    auto holdsCollections = [&](TypeID type) {
        return type && !isNeverType(type) && types.isSubset(type, collections);
    };
    if (Reference built = receiver < text ? walker.referenceOf(receiver) : Reference(); built && holdsCollections(walker.typeOf(built)))
        return true;
    TypeID expected = findExpectation(walker, receiver).type;
    if (!expected && walker.currentBodyIndex().opcodeOf(receiver) == Opcode::ElemGet)
        expected = walker.asFarAsUnitMayAsk(walker.facts().linked().resultOfReadByKey(walker.currentFunction(), receiver));
    return expected && holdsCollections(types.withoutNullish(expected));
}

static ValueID findKeyThatIsConvertedOnceInWalkTexts(const BodyIndex& index, ValueID text, const BodyValue& test)
{
    if (test.opcode() != Opcode::TypeTest || test.as<Opcode::TypeTest>().testedType() != TypeTable::object)
        return { };
    BlockID block = index.blockOf(test.id());
    ValueID whereItIsNoObject;
    index.forEachEdgeResult(block, [&](const BodyEdgeResult& result) {
        if (result.test == test.id() && result.subject == text && !result.isOnTakenEdge)
            whereItIsNoObject = result.parameter;
    });
    if (!whereItIsNoObject)
        return { };
    BodyTerminator end = index.terminator(index.blockOf(whereItIsNoObject));
    if (end.kind() != TerminatorKind::Jump || end.next().block().index() <= block.index())
        return { };
    BodyTarget next = end.next();
    std::span<const ValueID> handedOn = next.arguments();
    auto place = std::ranges::find(handedOn, whereItIsNoObject);
    return place == handedOn.end() ? ValueID() : index.block(next.block()).parameter(static_cast<uint32_t>(place - handedOn.begin()));
}

static UseOfTextAsKey useAsKeyInWalkTexts(Walker& walker, const BodyIndex& index, ValueID text, bool goesOnThroughJoinThatAloneTakesText = false, bool isAskedOfCallBeingCopied = false)
{
    Vector<ValueID, 8> passed;
    auto sayAtEnd = [&](UseOfTextAsKey use) {
        if (passed.size() > 1) {
            for (ValueID value : passed)
                walker.noteWhereUsesOfTextEnd(value, goesOnThroughJoinThatAloneTakesText, text);
        }
        return use;
    };
    while (true) {
        if (ValueID known = walker.whereUsesOfTextEnd(text, goesOnThroughJoinThatAloneTakesText))
            text = known;
        std::optional<BodyUser> mention = index.firstUser(text);
        TerminatorKind end = mention && mention->kind == BodyUseKind::TerminatorOperand ? index.terminator(mention->block).kind() : TerminatorKind::Unreachable;
        if ((end == TerminatorKind::Return || end == TerminatorKind::ReturnCall) && walker.useOfAnswerOfCopyAsKey() != UseOfTextAsKey::NotKnown)
            return sayAtEnd(walker.useOfAnswerOfCopyAsKey());
        if (end == TerminatorKind::Return && walker.facts().resultIsReadAsKey(walker.currentFunction()))
            return sayAtEnd(UseOfTextAsKey::AnyKey);
        if (!mention || mention->kind != BodyUseKind::Operand)
            return sayAtEnd(UseOfTextAsKey::NotKnown);
        BodyValue user = index.value(mention->value);
        if (accessesUnderKeysInWalkTexts.contains(user.opcode()))
            return sayAtEnd(user.operand(OperandIndex::ElemGet::property) == text ? UseOfTextAsKey::NameOfProperty : UseOfTextAsKey::None);
        if (ValueID converted = findKeyThatIsConvertedOnceInWalkTexts(index, text, user)) {
            passed.append(text);
            text = converted;
            continue;
        }
        if (!opcodesThatStateTypes.contains(user.opcode()) && user.opcode() != Opcode::ToPropertyKey && !(goesOnThroughJoinThatAloneTakesText && operationsThatJoinTexts.contains(user.opcode()) && index.onlyUsingValue(text) == user.id())) {
            if (takesKeyOfCollectionInWalkTexts(walker, user, text))
                return sayAtEnd(UseOfTextAsKey::AnyKey);
            return sayAtEnd(isAskedOfCallBeingCopied && !operationsThatJoinTexts.contains(user.opcode()) ? UseOfTextAsKey::NotKnown : UseOfTextAsKey::None);
        }
        passed.append(text);
        text = user.id();
    }
}

Walker::UseOfTextAsKey useOfAnswerOfCallBeingCopiedAsKey(Walker& walker, const BodyValue& call, const BodyIndex& copied)
{
    const BodyIndex& index = walker.currentBodyIndex();
    if (!copied.holdsAnyOf(operationsThatJoinTexts | OpcodeSet { Opcode::Call, Opcode::CallDirect }) || call.id().index() >= index.numberOfValues() || index.value(call.id()).words() != call.words())
        return UseOfTextAsKey::NotKnown;
    return useAsKeyInWalkTexts(walker, index, call.id(), false, true);
}

bool joinIsLikelyKeyInWalkTexts(Walker& walker, ValueID join)
{
    return useAsKeyInWalkTexts(walker, walker.currentBodyIndex(), join) > UseOfTextAsKey::None;
}

static bool takePartsInWalkTexts(Walker& walker, const BodyValue& join, ValueID joinInFront, std::span<const Reference> partsInFront, PartsOfJoinedText& parts, JoinsTakenApart* joinsThatFailWhereReaderFails)
{
    bool eachHasText = true;
    bool joinsTexts = join.opcode() == Opcode::Strcat;
    join.forEachOperand([&](ValueID operand, uint32_t) {
        if (operand == joinInFront) {
            parts.append(partsInFront);
            joinsTexts = true;
            return;
        }
        if (takePartsOfJoinMadeForOneReader(walker, operand, parts, joinsThatFailWhereReaderFails)) {
            joinsTexts = true;
            return;
        }
        Reference part = walker.referenceOf(operand);
        if (walker.hasStringsInBuilders() && isStringInBuilder(walker, part)) {
            joinsTexts = true;
            parts.append(part);
            return;
        }
        Type type = part ? walker.types().at(walker.typeOf(part)) : Type();
        eachHasText &= part && hasTextByItself(type);
        joinsTexts |= part && type.isAnyString();
        parts.append(part);
    });
    return eachHasText && joinsTexts;
}

static ValueID findJoinThatAloneTakesInWalkTexts(const Walker& walker, const BodyIndex& index, const BodyBlock& block, ValueID text)
{
    ValueID user = index.onlyUsingValue(text);
    if (!user || user.index() >= block.firstValue().index() + walker.endOfStretchBeingWalked() || !operationsThatJoinTexts.contains(index.opcodeOf(user)))
        return { };
    for (uint32_t between = text.index() + 1; between < user.index(); ++between) {
        Opcode opcode = index.opcodeOf(ValueID(between));
        if (!opcodesThatStateTypes.contains(opcode) && !declaredBoundInTable(opcode).isUnobservable())
            return { };
    }
    return user;
}

static void letJoinsFailWhereReaderFailsInWalkTexts(Walker& walker, const BodyValue& reader, std::span<const ValueID> joins)
{
    if (walker.isUnreachable() || walker.hasFailed() || !walker.referenceOf(reader.id()))
        return;
    for (ValueID join : joins)
        walker.code().letJoinFailWhereItsReaderFails(join);
}

struct TextsJoinedInFrontInWalkTexts {
    int64_t limit { 0 };
    PartsOfJoinedText texts;
    PartsOfJoinedText expectedToBeTexts;
};

static std::optional<TextsJoinedInFrontInWalkTexts> findTextsJoinedInFrontInWalkTexts(Walker& walker, std::span<const Reference> parts)
{
    TextsJoinedInFrontInWalkTexts found;
    int64_t room = Range::length().maximum;
    int64_t numberOfTexts = 0;
    for (Reference part : parts) {
        if (!walker.isInvariantInSpecializedVersion(part))
            return std::nullopt;
        if (StringID text = textKnownForStrings(walker, part)) {
            room -= lengthOfTextForStrings(walker, text);
            continue;
        }
        TypeID id = walker.typeOf(part);
        Type type = walker.types().at(id);
        if (!hasTextByItself(type) || type.primitives & Primitive::BigInt)
            return std::nullopt;
        if (!(type.primitives & Primitive::String)) {
            room -= static_cast<int64_t>(longestTextOf(walker.types(), type));
            continue;
        }
        bool isText = isProvenString(walker, id);
        if (!isText && !type.hasOnlyPrimitives(Primitive::AnyString | Primitive::UndefinedOrNull))
            return std::nullopt;
        ++numberOfTexts;
        PartsOfJoinedText& asked = isText ? found.texts : found.expectedToBeTexts;
        if (!asked.contains(part))
            asked.append(part);
    }
    if (!numberOfTexts || room < numberOfTexts)
        return std::nullopt;
    found.limit = static_cast<int64_t>(std::bit_floor(static_cast<uint64_t>(room / numberOfTexts)));
    return found;
}

static bool tryHoistConcatenationChecks(Walker& walker, TextsJoinedInFrontInWalkTexts& found, PartsOfJoinedText& parts, Origin origin)
{
    origin.isTailCall = false;
    if (!walker.tryHoistEntryTest())
        return false;
    for (Reference expected : found.expectedToBeTexts) {
        Reference isText;
        {
            Walker::SpecializedPreheaderScope preheaderScope(walker, true);
            isText = buildTypeTestForStrings(walker, expected, TypeTable::string, origin);
        }
        if (!isText || !walker.tryBranchInPreheader(isText, true))
            return false;
        Walker::SpecializedPreheaderScope preheaderScope(walker, true);
        Reference text = buildProofBehindTestForStrings(walker, expected, isText, OperandIndex::TypeTest::value, origin);
        if (!text)
            return false;
        std::ranges::replace(parts, expected, text);
        found.texts.append(text);
    }
    Reference areShort;
    {
        Walker::SpecializedPreheaderScope preheaderScope(walker, true);
        Reference lengths;
        for (Reference text : found.texts) {
            Reference length = buildLengthOfString(walker, text, origin);
            lengths = lengths && length ? buildOperatorForStrings(walker, Opcode::BitOr, lengths, length, origin) : length;
            if (!lengths)
                return false;
        }
        areShort = buildOperatorForStrings(walker, Opcode::Less, lengths, integerForStrings(walker, found.limit), origin);
    }
    return areShort && walker.tryBranchInPreheader(areShort, true);
}

static Reference buildJoinedTextInWalkTexts(Walker& walker, const BodyValue& join, std::span<const Reference> parts, UseOfTextAsKey use)
{
    PartsOfJoinedText merged(parts);
    mergeKnownPartsOfJoinedText(walker, merged);
    if (merged.size() == 1 && walker.types().at(walker.typeOf(merged[0])).isAnyString())
        return merged[0];
    std::optional<TextsJoinedInFrontInWalkTexts> inFrontOfLoop = findTextsJoinedInFrontInWalkTexts(walker, merged.span());
    bool isMadeInFront = inFrontOfLoop && tryHoistConcatenationChecks(walker, *inFrontOfLoop, merged, walker.originOf(join));
    Draft joined(Opcode::Strcat, walker.originOf(join));
    joined.inputValueGiven = join.id();
    joined.setPartsOperands(merged.span());
    walker.chooseConservativeFields(joined);
    joined.setIsLikelyKey(use > UseOfTextAsKey::None);
    joined.setIsLikelyName(use == UseOfTextAsKey::NameOfProperty);
    std::optional<Draft> joinedAgain = isMadeInFront && walker.checksHoistedCalls() ? std::optional<Draft>(joined) : std::nullopt;
    Reference built;
    {
        Walker::SpecializedPreheaderScope preheaderScope(walker, isMadeInFront);
        built = walker.build(WTF::move(joined));
    }
    if (joinedAgain)
        walker.holdLookupInFrontAgainstLookupInTurn(built, WTF::move(*joinedAgain), declaredBoundInTable(Opcode::Strcat), ProofSource::HoistedCall);
    return built;
}

static void bindJoinedTextInWalkTexts(Walker& walker, const BodyValue& join, std::span<const Reference> parts, UseOfTextAsKey use)
{
    if (Reference text = buildJoinedTextInWalkTexts(walker, join, parts, use))
        walker.bind(join.id(), text);
}

static bool bindKeyOfTextAndValueInWalkTexts(Walker& walker, const BodyIndex& index, const BodyValue& sum)
{
    if (sum.opcode() != Opcode::Add)
        return false;
    std::array<Reference, 2> addends { walker.referenceOf(sum.operand(0)), walker.referenceOf(sum.operand(1)) };
    if (!addends[0] || !addends[1])
        return false;
    if (walker.hasStringsInBuilders() && (isStringInBuilder(walker, addends[0]) || isStringInBuilder(walker, addends[1])))
        return false;
    bool textIsOnLeft = isProvenString(walker, walker.typeOf(addends[0]));
    if (textIsOnLeft == isProvenString(walker, walker.typeOf(addends[1])))
        return false;
    UseOfTextAsKey use = useAsKeyInWalkTexts(walker, index, sum.id(), true);
    if (use <= UseOfTextAsKey::None)
        return false;
    Origin origin = walker.originOf(sum);
    origin.isTailCall = false;
    Reference value = addends[textIsOnLeft];
    Reference hasText = buildTypeTestForStrings(walker, value, typeOfPrimitives(Primitive::NumberLike | Primitive::String), origin);
    std::array<Reference, 1> key;
    bool isBuilt = buildArmsForStrings(walker, hasText, BlockFrequency::Rare, [&](std::span<Reference> results) {
        std::array<Reference, 2> parts = addends;
        parts[textIsOnLeft] = buildProofBehindTestForStrings(walker, value, hasText, OperandIndex::TypeTest::value, origin);
        results[0] = parts[textIsOnLeft] ? buildJoinedTextInWalkTexts(walker, sum, parts, use) : Reference();
    }, [&](std::span<Reference> results) {
        results[0] = buildOperatorForStrings(walker, Opcode::Add, addends[0], addends[1], origin);
    }, key);
    if (isBuilt)
        walker.bind(sum.id(), key[0]);
    return true;
}

static_assert(opcodesThatBuildTypedJoinOfTextsTakes == (operationsThatJoinTexts));

bool buildTypedJoinOfTextsOfItsOpcodes(Walker& walker, const BodyValue& first)
{
    if (!operationsThatJoinTexts.contains(first.opcode()))
        return false;
    const BodyIndex& index = walker.currentBodyIndex();

    PartsOfJoinedText parts;
    JoinsTakenApart joinsThatFailWhereReaderFails;
    if (!takePartsInWalkTexts(walker, first, { }, { }, parts, &joinsThatFailWhereReaderFails)) {
        if (!joinsThatFailWhereReaderFails.isEmpty()) {
            parts.clear();
            takePartsInWalkTexts(walker, first, { }, { }, parts, nullptr);
        }
        if (tryConvertAndAppendParts(walker, first, parts.span()))
            return true;
        UseOfTextAsKey use = first.opcode() == Opcode::Strcat && !parts.contains(Reference()) ? useAsKeyInWalkTexts(walker, index, first.id()) : UseOfTextAsKey::None;
        if (use <= UseOfTextAsKey::None)
            return bindKeyOfTextAndValueInWalkTexts(walker, index, first);
        bindJoinedTextInWalkTexts(walker, first, parts.span(), use);
        return true;
    }

    BodyBlock block = index.block(index.blockOf(first.id()));
    uint32_t firstValue = block.firstValue().index();
    BodyValue last = first;
    bool hasWalkedOn = false;
    while (ValueID taker = findJoinThatAloneTakesInWalkTexts(walker, index, block, last.id())) {
        walker.walkStretch(block.id(), last.id().index() + 1 - firstValue, taker.index() - firstValue);
        hasWalkedOn = true;
        if (walker.isUnreachable() || walker.hasFailed())
            return true;
        PartsOfJoinedText longer;
        JoinsTakenApart moreJoinsThatFailWhereReaderFails(joinsThatFailWhereReaderFails);
        if (!takePartsInWalkTexts(walker, index.value(taker), last.id(), parts.span(), longer, &moreJoinsThatFailWhereReaderFails) || longer.size() > mostPartsOfJoinedText)
            break;
        parts = WTF::move(longer);
        joinsThatFailWhereReaderFails = WTF::move(moreJoinsThatFailWhereReaderFails);
        last = index.value(taker);
        if (FunctionID callee = walker.facts().linked().calleeAt(walker.currentFunction(), last.id()))
            walker.leaveAnythingAt(callee);
    }

    if (walker.hasStringsInBuilders() && tryAppendJoinToStringInBuilder(walker, last, parts)) {
        letJoinsFailWhereReaderFailsInWalkTexts(walker, last, joinsThatFailWhereReaderFails.span());
        uint32_t behindGrowth = last.id().index() + 1 - firstValue;
        walker.walkStretch(block.id(), behindGrowth, behindGrowth);
        return true;
    }
    UseOfTextAsKey use = useAsKeyInWalkTexts(walker, index, last.id(), parts.size() == 2);
    hasWalkedOn |= holdsPartsTakenOutOfTexts(walker, last, parts.span());
    if (!hasWalkedOn && use <= UseOfTextAsKey::None)
        return false;
    bindJoinedTextInWalkTexts(walker, last, parts.span(), use);
    letJoinsFailWhereReaderFailsInWalkTexts(walker, last, joinsThatFailWhereReaderFails.span());
    uint32_t behindRow = last.id().index() + 1 - firstValue;
    walker.walkStretch(block.id(), behindRow, behindRow);
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
