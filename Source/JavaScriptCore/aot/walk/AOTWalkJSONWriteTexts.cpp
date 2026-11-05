/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalkJSONWrites.h"

#if ENABLE(AOT)

#include "AOTEffectsDeclared.h"
#include "AOTEffectsOfOperations.h"
#include "AOTFacts.h"
#include "AOTLinkTables.h"
#include "AOTOpcodeSets.h"
#include "AOTRepresentationRules.h"
#include "AOTTypeArena.h"
#include "AOTWalkTextBuilders.h"
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr size_t mostPartsAroundJSONText = 32;

namespace {

struct RowAroundJSONText {
    Vector<ValueID, 4> joins;
    Vector<ValueID, 8> parts;
    ValueID conversion;
    bool growsTextInBuilder { false };
};

} // anonymous namespace

static ValueID findJoinThatAloneTakesInJSONWriteTexts(const Walker& walker, const BodyIndex& index, const BodyBlock& block, ValueID text)
{
    ValueID user = index.hasOneUse(text) ? index.onlyUsingValue(text) : ValueID();
    if (!user || user.index() >= block.firstValue().index() + walker.endOfStretchBeingWalked() || !operationsThatJoinTexts.contains(index.opcodeOf(user)))
        return { };
    for (uint32_t between = text.index() + 1; between < user.index(); ++between) {
        Opcode opcode = index.opcodeOf(ValueID(between));
        if (!opcodesThatStateTypes.contains(opcode) && !constantsInFormOfUses.contains(opcode) && !declaredBoundInTable(opcode).isPure())
            return { };
    }
    return user;
}

static TypeID typeOfPartInJSONWriteTexts(const Walker& walker, const BodyIndex& index, ValueID part, ValueID call)
{
    if (part < call) {
        Reference built = walker.referenceOf(part);
        return built ? walker.typeOf(built) : TypeID();
    }
    switch (index.opcodeOf(part)) {
    case Opcode::String:
        return TypeTable::string;
    case Opcode::Integer:
    case Opcode::Double:
        return TypeTable::number;
    case Opcode::Boolean:
        return TypeTable::boolean;
    case Opcode::Null:
        return TypeTable::null;
    case Opcode::Undefined:
        return TypeTable::undefined;
    default:
        return { };
    }
}

static std::optional<RowAroundJSONText> findRowAroundJSONText(Walker& walker, const BodyIndex& index, const BodyBlock& block, const BodyValue& call)
{
    RowAroundJSONText row;
    row.parts.append(call.id());
    ValueID text = call.id();
    ValueID onlyUser = index.hasOneUse(text) ? index.onlyUsingValue(text) : ValueID();
    if (onlyUser && onlyUser.index() == text.index() + 1 && index.opcodeOf(onlyUser) == Opcode::ToString) {
        row.conversion = onlyUser;
        text = onlyUser;
    }
    while (ValueID taker = findJoinThatAloneTakesInJSONWriteTexts(walker, index, block, text)) {
        BodyValue join = index.value(taker);
        Vector<ValueID, 8> longer;
        bool joinsTexts = join.opcode() == Opcode::Strcat || text != call.id();
        bool eachHasText = true;
        bool growsTextInBuilder = false;
        join.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
            if (operand == text) {
                longer.appendVector(row.parts);
                return;
            }
            longer.append(operand);
            if (walker.hasTextsInStringBuilders() && operand < call.id() && isNameOfTextInStringBuilder(walker, operand)) {
                growsTextInBuilder = !operandIndex && isGrowthOfTextInStringBuilder(walker, join);
                eachHasText &= growsTextInBuilder;
                return;
            }
            TypeID proven = typeOfPartInJSONWriteTexts(walker, index, operand, call.id());
            Type type = proven ? walker.types().at(proven) : Type();
            eachHasText &= proven && type.isJavaScriptValue() && !type.hasObjects() && hasTextByItself(type);
            joinsTexts |= proven && type.isAnyString();
        });
        if (!eachHasText || !(joinsTexts || growsTextInBuilder) || longer.size() > mostPartsAroundJSONText)
            break;
        row.joins.append(taker);
        row.parts = WTF::move(longer);
        text = taker;
        if (growsTextInBuilder) {
            row.growsTextInBuilder = true;
            break;
        }
    }
    if (row.joins.isEmpty())
        return std::nullopt;
    return row;
}

static void writeJSONTextThroughCellOfBuilder(Walker& walker, const BodyValue& call, const PlanOfJSONCall& planned, TextInStringBuilder& text, Reference name, Origin origin)
{
    const BodyIndex& index = walker.currentBodyIndex();
    BodyBlock block = index.block(index.blockOf(call.id()));
    uint32_t place = call.id().index() - block.firstValue().index();

    walker.noteGuessOpened();
    text.stringOfState = { };
    text.lengthOfState = { };
    JSONTextWriter writer(walker, origin);
    if (!writer.begin(name, planned.roomForMembers))
        return;
    writer.writeRoot(walker.referenceOf(planned.source), planned.plan, planned.callsSerializers);
    if (walker.hasFailed())
        return;
    BlockID endOfTypedArm = walker.isUnreachable() ? BlockID() : walker.currentBlock();

    BlockID endOfClosedArm;
    if (writer.enterSlowPath()) {
        Walker::ClosedArm closedArm(walker);
        walker.walkStretch(block.id(), place, place + 1);
        if (!walker.isUnreachable() && !walker.hasFailed()) {
            Reference result = walker.referenceOf(call.id());
            appendPartsToStringBuilder(walker, text, name, singleElementSpan(result), origin);
            if (!walker.isUnreachable())
                endOfClosedArm = walker.currentBlock();
        }
    }
    if (walker.hasFailed())
        return;

    if (!endOfTypedArm || !endOfClosedArm) {
        walker.continueIn(endOfTypedArm ? endOfTypedArm : endOfClosedArm);
        return;
    }
    BlockID meeting = walker.addBlock();
    walker.continueIn(endOfTypedArm);
    walker.jumpTo(meeting);
    walker.continueIn(endOfClosedArm);
    walker.jumpTo(meeting);
    walker.continueIn(meeting);
}

static bool takesJSONTextInJSONWriteTexts(const Walker& walker, const TextInStringBuilder& text, Reference name)
{
    return isTextInStringBuilder(walker, name) && !text.liesInFrame && !text.web.standsUnderHandlers;
}

bool writeJSONTextIntoJoinFurtherOn(Walker& walker, const BodyValue& call, const PlanOfJSONCall& planned)
{
    const BodyIndex& index = walker.currentBodyIndex();
    BodyBlock block = index.block(index.blockOf(call.id()));
    std::optional<RowAroundJSONText> row = findRowAroundJSONText(walker, index, block, call);
    if (!row)
        return false;

    TextInStringBuilder* growing = nullptr;
    Reference name;
    if (row->growsTextInBuilder) {
        growing = findTextNamedBy(walker, row->parts[0]);
        name = walker.referenceBoundTo(row->parts[0]);
        if (!growing || !takesJSONTextInJSONWriteTexts(walker, *growing, name))
            return false;
    }
    Origin origin = walker.originOf(call);
    origin.isTailCall = false;

    ValueID last = row->joins.last();
    uint32_t firstValue = block.firstValue().index();
    for (uint32_t between = call.id().index() + 1; between < last.index(); ++between) {
        if (row->joins.contains(ValueID(between)) || ValueID(between) == row->conversion)
            continue;
        walker.walkStretch(block.id(), between - firstValue, between + 1 - firstValue);
        if (walker.isUnreachable() || walker.hasFailed())
            return true;
    }

    Vector<Reference, 8> partsInFront;
    Vector<Reference, 8> partsBehind;
    bool standsBehindCall = false;
    for (size_t place = growing ? 1 : 0; place < row->parts.size(); ++place) {
        if (row->parts[place] == call.id()) {
            standsBehindCall = true;
            continue;
        }
        Reference part = walker.referenceOf(row->parts[place]);
        if (!part) {
            walker.fail("a part of the row of joins around a JSON text was not built"_s);
            return true;
        }
        (standsBehindCall ? partsBehind : partsInFront).append(part);
    }

    TextInStringBuilder own;
    if (!growing) {
        Draft begin(Opcode::StringBuilderBegin, origin);
        walker.chooseConservativeFields(begin);
        name = walker.build(WTF::move(begin));
        if (!name)
            return true;
        own.builder = name;
        own.beganEmpty = true;
    }
    TextInStringBuilder& text = growing ? *growing : own;

    appendPartsToStringBuilder(walker, text, name, partsInFront.span(), origin);
    if (!walker.isUnreachable())
        writeJSONTextThroughCellOfBuilder(walker, call, planned, text, name, origin);
    if (!walker.isUnreachable())
        appendPartsToStringBuilder(walker, text, name, partsBehind.span(), origin);
    if (walker.isUnreachable() || walker.hasFailed())
        return true;

    if (growing)
        walker.bind(last, name);
    else {
        Draft finish(Opcode::StringBuilderFinish, origin);
        walker.chooseConservativeFields(finish);
        finish.setBuilderOperand(name);
        finish.inputValueGiven = last;
        if (Reference joined = walker.build(WTF::move(finish)))
            walker.bind(last, joined);
    }

    for (ValueID join : row->joins) {
        if (FunctionID callee = walker.facts().linked().calleeAt(walker.currentFunction(), join))
            walker.arrivals().leaveAnything(callee);
    }
    if (FunctionID callee = row->conversion ? walker.facts().linked().calleeAt(walker.currentFunction(), row->conversion) : FunctionID())
        walker.arrivals().leaveAnything(callee);
    uint32_t behindRow = last.index() + 1 - firstValue;
    walker.walkStretch(block.id(), behindRow, behindRow);
    return true;
}

bool appendJSONTextStraight(Walker& walker, TextInStringBuilder& text, Reference name, ValueID part, Origin origin)
{
    const BodyIndex& index = walker.currentBodyIndex();
    if (walker.isUnreachable() || walker.referenceBoundTo(part) || index.isParameter(part) || !index.hasOneUse(part) || !takesJSONTextInJSONWriteTexts(walker, text, name))
        return false;
    BodyValue call = index.value(part);
    std::optional<PlanOfJSONCall> planned = planJSONTextOfCall(walker, call, InputsOfTypedOperation(walker, nullptr));
    if (!planned || !paysToWriteJSONText(walker, *planned))
        return false;
    origin.isTailCall = false;
    writeJSONTextThroughCellOfBuilder(walker, call, *planned, text, name, origin);
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
