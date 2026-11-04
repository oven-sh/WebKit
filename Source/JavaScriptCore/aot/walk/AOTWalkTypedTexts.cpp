/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// TypedOperations, texts that are joined: `a + b + c`, `text += a + b`, a template. A row of joins, each of which alone takes what the one in front of it gives, is one Strcat of all the parts where each part is proven to have text by itself.
// A text whose first reader takes it as a key is made as a key: the run time has joined the same parts before, and hands out the name that it made then.
// It keeps nothing from one operation to the next: the first join of a row walks the row to its end. At a suspension: no row goes past one. At a throw: nothing that the program can see stands between two joins of a row,
// so a text that is too long is refused at the last of them as it would be at the first. Withheld: each join is copied in its closed form.

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
#include "AOTWalkKnown.h"
#include <optional>
#include <span>

namespace JSC { namespace AOT { namespace Typed {

using PartsOfJoinedText = Vector<Reference, 8>;

// One Strcat lists no more parts than this: its emitter keeps the text of every one of them live at once. A rope has three parts, so a longer row makes no more ropes as two operations than as one.
static constexpr size_t mostPartsOfJoinedText = 32;

static constexpr OpcodeSet accessesUnderKeysInWalkTexts { Opcode::ElemGet, Opcode::ElemSet, Opcode::ElemHas, Opcode::ElemDelete, Opcode::ElemDefine };
static_assert(OperandIndex::ElemGet::property == OperandIndex::ElemSet::property && OperandIndex::ElemGet::property == OperandIndex::ElemHas::property);
static_assert(OperandIndex::ElemGet::property == OperandIndex::ElemDelete::property && OperandIndex::ElemGet::property == OperandIndex::ElemDefine::property);

namespace {

enum class UseOfTextAsKey : uint8_t { None, AnyKey, NameOfProperty };

} // anonymous namespace

// ---- Keys

// Every method of a Map or a Set that takes a text at all takes it first, as the key.
static bool takesKeyOfCollectionInWalkTexts(Walker& walker, const BodyValue& user, ValueID text)
{
    if (user.opcode() != Opcode::Call)
        return false;
    auto call = user.as<Opcode::Call>();
    auto arguments = call.argumentsOperands();
    ValueID receiver = call.thisValueOperand();
    if (!arguments.size() || *arguments.begin() != text || !receiver || !(receiver < text))
        return false;

    TypeArena& types = walker.types();
    TypeID collections = types.unionOf(types.builtinClass(BuiltinClasses::Map), types.builtinClass(BuiltinClasses::Set));
    auto holdsCollections = [&](TypeID type) {
        return type && !isNeverType(type) && types.isSubset(type, collections);
    };
    if (Reference built = walker.referenceOf(receiver); built && holdsCollections(walker.typeOf(built)))
        return true;
    TypeID expected = findExpectation(walker, receiver).type;
    return expected && holdsCollections(types.withoutNullish(expected));
}

// By the first mention of the text in the stream, behind whatever only hands it on. It decides what pays, never what is right: a key is a string like any other.
static UseOfTextAsKey useAsKeyInWalkTexts(Walker& walker, const BodyIndex& index, ValueID text)
{
    while (true) {
        std::optional<BodyUser> mention = index.firstUser(text);
        // `return `${a.id},${b.id}`;`: who reads it stands in another body, and Facts has seen one that looks something up by it.
        if (mention && mention->kind == BodyUseKind::TerminatorOperand && index.block(mention->block).terminator().kind() == TerminatorKind::Return && walker.facts().resultIsReadAsKey(walker.currentFunction()))
            return UseOfTextAsKey::AnyKey;
        if (!mention || mention->kind != BodyUseKind::Operand)
            return UseOfTextAsKey::None;
        BodyValue user = index.value(mention->value);
        if (accessesUnderKeysInWalkTexts.contains(user.opcode()))
            return user.operand(OperandIndex::ElemGet::property) == text ? UseOfTextAsKey::NameOfProperty : UseOfTextAsKey::None;
        if (!opcodesThatStateTypes.contains(user.opcode()))
            return takesKeyOfCollectionInWalkTexts(walker, user, text) ? UseOfTextAsKey::AnyKey : UseOfTextAsKey::None;
        text = user.id();
    }
}

// For guessThatOperandBesideStringHasTextOfItsOpcodes(), which stands in front of the join.
bool joinIsLikelyKeyInWalkTexts(Walker& walker, ValueID join)
{
    return useAsKeyInWalkTexts(walker, walker.currentBodyIndex(), join) != UseOfTextAsKey::None;
}

// ---- Rows

// The parts of the join in the order in which its text holds them. In the place of `joinInFront`, which it alone takes, stand the parts of that.
// False: a part may run code to give its text, or the operation may add numbers.
static bool takePartsInWalkTexts(Walker& walker, const BodyValue& join, ValueID joinInFront, std::span<const Reference> partsInFront, PartsOfJoinedText& parts)
{
    bool eachHasText = true;
    bool joinsTexts = join.opcode() == Opcode::Strcat;
    join.forEachOperand([&](ValueID operand, uint32_t) {
        if (operand == joinInFront) {
            parts.append(partsInFront);
            joinsTexts = true;
            return;
        }
        if (takePartsOfJoinReturnedByCopy(walker, operand, parts)) {
            joinsTexts = true;
            return;
        }
        Reference part = walker.referenceOf(operand);
        if (walker.hasTextsInStringBuilders() && isTextInStringBuilder(walker, part)) {
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

// The join that alone takes the text, further on in the stretch that is being walked. Null: there is none, or something stands between the two that the program can see.
static ValueID findJoinThatAloneTakesInWalkTexts(const Walker& walker, const BodyIndex& index, const BodyBlock& block, ValueID text)
{
    ValueID user = index.onlyUsingValue(text);
    if (!user || user.index() >= block.firstValue().index() + walker.endOfStretchBeingWalked() || !operationsThatJoinTexts.contains(index.opcodeOf(user)))
        return { };
    for (uint32_t between = text.index() + 1; between < user.index(); ++between) {
        Opcode opcode = index.opcodeOf(ValueID(between));
        if (mayStandInsideRowOfJoins(walker, index, text, ValueID(between)))
            continue;
        if (!opcodesThatStateTypes.contains(opcode) && !declaredBoundInTable(opcode).isUnobservable())
            return { };
    }
    return user;
}

static void bindJoinedTextInWalkTexts(Walker& walker, const BodyValue& join, std::span<const Reference> parts, UseOfTextAsKey use)
{
    PartsOfJoinedText merged(parts);
    mergeKnownPartsOfJoinedText(walker, merged);
    if (merged.size() == 1 && walker.types().at(walker.typeOf(merged[0])).isAnyString()) {
        walker.bind(join.id(), merged[0]);
        return;
    }
    Draft joined(Opcode::Strcat, walker.originOf(join));
    joined.inputValueGiven = join.id();
    joined.setPartsOperands(merged.span());
    walker.chooseConservativeFields(joined);
    joined.setIsLikelyKey(use != UseOfTextAsKey::None);
    joined.setIsLikelyName(use == UseOfTextAsKey::NameOfProperty);
    std::optional<Effects> ofKnownTexts = effectsOfJoinOfKnownTexts(walker, joined);
    if (Reference text = walker.build(WTF::move(joined), ofKnownTexts ? &*ofKnownTexts : nullptr))
        walker.bind(join.id(), text);
}

static_assert(opcodesThatBuildTypedJoinOfTextsTakes == (operationsThatJoinTexts));

bool buildTypedJoinOfTextsOfItsOpcodes(Walker& walker, const BodyValue& first)
{
    if (!operationsThatJoinTexts.contains(first.opcode()))
        return false;
    const BodyIndex& index = walker.currentBodyIndex();

    PartsOfJoinedText parts;
    if (!takePartsInWalkTexts(walker, first, { }, { }, parts)) {
        if (convertPartsAndAppendToTextInStringBuilder(walker, first, parts.span()))
            return true;
        // The emitter converts each part of a template that may run code where it stands, and hands the texts to the run time.
        UseOfTextAsKey use = first.opcode() == Opcode::Strcat && !parts.contains(Reference()) ? useAsKeyInWalkTexts(walker, index, first.id()) : UseOfTextAsKey::None;
        if (use == UseOfTextAsKey::None)
            return false;
        bindJoinedTextInWalkTexts(walker, first, parts.span(), use);
        return true;
    }

    bool hasTakenTextsApart = takePartsOfTextsThatJoinAloneReads(walker, first, parts, mostPartsOfJoinedText);
    BodyBlock block = index.block(index.blockOf(first.id()));
    uint32_t firstValue = block.firstValue().index();
    BodyValue last = first;
    bool hasWalkedOn = false;
    auto findTaker = [&] {
        ValueID taker = findJoinThatAloneTakesInWalkTexts(walker, index, block, last.id());
        return taker ? taker : findJoinThatTakesLinkPastCode(walker, last.id(), parts.span());
    };
    while (ValueID taker = findTaker()) {
        walker.walkStretch(block.id(), last.id().index() + 1 - firstValue, taker.index() - firstValue);
        hasWalkedOn = true;
        if (walker.isUnreachable() || walker.hasFailed())
            return true;
        PartsOfJoinedText longer;
        if (!takePartsInWalkTexts(walker, index.value(taker), last.id(), parts.span(), longer) || longer.size() > mostPartsOfJoinedText)
            break;
        parts = WTF::move(longer);
        last = index.value(taker);
        // No part runs code, so a call that Link lists here for a conversion enters nothing. It is told all the same, as Walker::walkOperation() tells it of a join that it walks.
        if (FunctionID callee = walker.facts().linked().calleeAt(walker.currentFunction(), last.id()))
            walker.arrivals().leaveAnything(callee);
    }

    if (walker.hasTextsInStringBuilders() && appendRowToTextInStringBuilder(walker, last, parts)) {
        uint32_t behindGrowth = last.id().index() + 1 - firstValue;
        walker.walkStretch(block.id(), behindGrowth, behindGrowth);
        return true;
    }
    UseOfTextAsKey use = useAsKeyInWalkTexts(walker, index, last.id());
    hasWalkedOn |= holdsPartsTakenOutOfTexts(walker, last, parts.span());
    if (!hasWalkedOn && !hasTakenTextsApart && use == UseOfTextAsKey::None)
        return false;
    bindJoinedTextInWalkTexts(walker, last, parts.span(), use);
    // The row is walked.
    uint32_t behindRow = last.id().index() + 1 - firstValue;
    walker.walkStretch(block.id(), behindRow, behindRow);
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
