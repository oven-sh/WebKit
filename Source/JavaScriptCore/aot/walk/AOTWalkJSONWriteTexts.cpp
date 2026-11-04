/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Where the text of JSON.stringify goes: `out += JSON.stringify(row) + "\n"` writes it behind what the builder of `out` holds, `"data: " + JSON.stringify(e) + "\n\n"` into one builder for the whole row. No string is made for it, and none is copied.
// No value waits: the call looks ahead in its stretch, walks up to the last join of the row and builds the row there. Where a test fails the builder is put back to what it held in front of the text, and what the call as written gives is appended.
// At a suspension: no row goes past one. At a throw: a text that is handed to a handler of the program takes no JSON text. Withheld: the call goes by its row, and the joins by theirs.

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

// As one Strcat lists.
static constexpr size_t mostPartsAroundJSONText = 32;

// The table of built-ins has no word for a function that serializes an operand and has a form that takes the text (BuiltinForms::jsonTextForm, jsonTextArgument), and the host has no such form.
bool handJSONTextToBuiltin(Walker&, const BodyValue&)
{
    constexpr bool handsJSONTextToBuiltins = true;
    if (!handsJSONTextToBuiltins)
        return false;
    return false;
}

namespace {

struct RowAroundJSONText {
    // The first first. The value of the last is the text of all the parts.
    Vector<ValueID, 4> joins;
    // In the order in which the text holds them. The call stands among them, once.
    Vector<ValueID, 8> parts;
    // `${JSON.stringify(e)}`: the ToString that stands right behind the call and alone takes what it gives. Null: there is none.
    ValueID conversion;
    // parts[0] names a text in a builder, and the last join is a growth of it.
    bool growsTextInBuilder { false };
};

} // anonymous namespace

// The join that alone takes the text, further on in the stretch that is being walked. The call as written may run any code, and it runs where the row is built.
// So what stands between reads nothing and does nothing: a constant, a cast. Null: there is none.
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

// By what the walk has built for it, or, of a constant that is written behind the call, by its opcode. Null: nobody can tell yet.
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

// It builds nothing.
static std::optional<RowAroundJSONText> findRowAroundJSONText(Walker& walker, const BodyIndex& index, const BodyBlock& block, const BodyValue& call)
{
    RowAroundJSONText row;
    row.parts.append(call.id());
    ValueID text = call.id();
    // Of a string or undefined it gives what a join writes, and it runs nothing.
    ValueID onlyUser = index.hasOneUse(text) ? index.onlyUsingValue(text) : ValueID();
    if (onlyUser && onlyUser.index() == text.index() + 1 && index.opcodeOf(onlyUser) == Opcode::ToString) {
        row.conversion = onlyUser;
        text = onlyUser;
    }
    while (ValueID taker = findJoinThatAloneTakesInJSONWriteTexts(walker, index, block, text)) {
        BodyValue join = index.value(taker);
        Vector<ValueID, 8> longer;
        // What a join in front gives is a text. What the call gives may be undefined, which an Add beside a number adds.
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
                // Whoever is no growth of the text reads it, and gets a string.
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
        // What a growth gives is a name of the text, which the walk of the loop follows.
        if (growsTextInBuilder) {
            row.growsTextInBuilder = true;
            break;
        }
    }
    if (row.joins.isEmpty())
        return std::nullopt;
    return row;
}

// The text is written through the cell of the builder, behind what it holds. The walk goes on where both ways have met.
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
            // A join writes undefined as "undefined".
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

// A builder in the frame has no cell. Under a handler that is handed the text, a throw half way would leave a part of the JSON text behind what the handler goes on with.
static bool takesJSONTextInJSONWriteTexts(const Walker& walker, const TextInStringBuilder& text, Reference name)
{
    return isTextInStringBuilder(walker, name) && !text.liesInFrame && !text.web.standsUnderHandlers;
}

bool writeJSONTextIntoJoinFurtherOn(Walker& walker, const BodyValue& call, const PlanOfJSONCall& planned)
{
    constexpr bool joinsTakeJSONTexts = true;
    if (!joinsTakeJSONTexts)
        return false;
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

    // What stands between the call and the last join, but for the joins of the row.
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

    // No part runs code, so a call that Link lists at a join for a conversion enters nothing. It is told all the same, as Walker::walkOperation() tells it of a join that it walks.
    for (ValueID join : row->joins) {
        if (FunctionID callee = walker.facts().linked().calleeAt(walker.currentFunction(), join))
            walker.arrivals().leaveAnything(callee);
    }
    if (FunctionID callee = row->conversion ? walker.facts().linked().calleeAt(walker.currentFunction(), row->conversion) : FunctionID())
        walker.arrivals().leaveAnything(callee);
    // The row is walked.
    uint32_t behindRow = last.index() + 1 - firstValue;
    walker.walkStretch(block.id(), behindRow, behindRow);
    return true;
}

bool appendJSONTextStraight(Walker& walker, TextInStringBuilder& text, Reference name, ValueID part, Origin origin)
{
    // A call that was built where it stands has given a string, which is appended like any other.
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
