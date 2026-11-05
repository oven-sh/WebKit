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
#include "AOTWalkKnown.h"
#include <optional>
#include <span>

namespace JSC { namespace AOT { namespace Typed {

using PartsOfJoinedText = Vector<Reference, 8>;

static constexpr size_t mostPartsOfJoinedText = 32;

static constexpr OpcodeSet accessesUnderKeysInWalkTexts { Opcode::ElemGet, Opcode::ElemSet, Opcode::ElemHas, Opcode::ElemDelete, Opcode::ElemDefine };
static_assert(OperandIndex::ElemGet::property == OperandIndex::ElemSet::property && OperandIndex::ElemGet::property == OperandIndex::ElemHas::property);
static_assert(OperandIndex::ElemGet::property == OperandIndex::ElemDelete::property && OperandIndex::ElemGet::property == OperandIndex::ElemDefine::property);

namespace {

enum class UseOfTextAsKey : uint8_t { None, AnyKey, NameOfProperty };

} // anonymous namespace

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

static UseOfTextAsKey useAsKeyInWalkTexts(Walker& walker, const BodyIndex& index, ValueID text)
{
    while (true) {
        std::optional<BodyUser> mention = index.firstUser(text);
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

bool joinIsLikelyKeyInWalkTexts(Walker& walker, ValueID join)
{
    return useAsKeyInWalkTexts(walker, walker.currentBodyIndex(), join) != UseOfTextAsKey::None;
}

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
    if (Reference text = walker.build(WTF::move(joined)))
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
        UseOfTextAsKey use = first.opcode() == Opcode::Strcat && !parts.contains(Reference()) ? useAsKeyInWalkTexts(walker, index, first.id()) : UseOfTextAsKey::None;
        if (use == UseOfTextAsKey::None)
            return false;
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
        if (!takePartsInWalkTexts(walker, index.value(taker), last.id(), parts.span(), longer) || longer.size() > mostPartsOfJoinedText)
            break;
        parts = WTF::move(longer);
        last = index.value(taker);
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
    if (!hasWalkedOn && use == UseOfTextAsKey::None)
        return false;
    bindJoinedTextInWalkTexts(walker, last, parts.span(), use);
    uint32_t behindRow = last.id().index() + 1 - firstValue;
    walker.walkStretch(block.id(), behindRow, behindRow);
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
