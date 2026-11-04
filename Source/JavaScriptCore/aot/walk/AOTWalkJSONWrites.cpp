/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// BuiltinCalls with TypedOperations and Guesses: `JSON.stringify(row)` without a replacer and an indent, where the types say what the value reaches. The call, the serializer unit of a layout, the tests, the arms and the cursor.
// At a suspension: nothing that is written here suspends, and no builder of a JSON text lives past the call. At a throw: a text that calls serializers stands under a handler of its own, which drops the text and runs the call as written.
// Withheld, in a slow version and in a closed arm: the call goes by its row.

#include "config.h"
#include "AOTWalkJSONWrites.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDs.h"
#include "AOTBuiltinTable.h"
#include "AOTCallingConventions.h"
#include "AOTFacts.h"
#include "AOTFamilies.h"
#include "AOTIRProgram.h"
#include "AOTKeyOrders.h"
#include "AOTPlan.h"
#include "AOTStringBuilderCursor.h"
#include "AOTStringBuilderWords.h"
#include "AOTSweep.h"
#include "AOTTypeArena.h"
#include <algorithm>
#include <utility>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

// AOTWalkTypedOperators.cpp.
OperatorForm formAllowedForTypedOperator(Walker&, const Draft&);

// A reservation is a compare and a branch. One of this size covers some ten short members.
static constexpr uint32_t leastBytesReservedForJSONText = 512;
static constexpr uint32_t outcomeResultOfJSONAppendValue = 0;
static constexpr uint32_t cursorResultOfJSONAppendValue = 1;
// Closed, the call enters the host, which finds the keys of each object and looks at each value to learn what it is.
static constexpr uint32_t closedOperationsOfJSONStringify = 2;
// A member that is written out costs its 30 to 80 bytes at every site. Where the code does not come round, a call of the serializer stands in the place of more members than a call costs.
static constexpr uint32_t mostJSONMembersWrittenOutOutsideLoops = 4;

static_assert(leastBytesReservedForJSONText <= maxStringBuilderReservation);
static_assert(mostJSONMembersWrittenOutOutsideLoops <= mostJSONMembersWrittenOutInOneUnit);

// ---- Operations

JSONTextWriter::JSONTextWriter(Walker& walker, Origin origin)
    : m_walker(walker)
    , m_context { walker.program(), WalkFacts(walker.facts()), walker.plan(), walker.types(), walker.currentFunction() }
    , m_origin(origin)
{
    m_origin.isTailCall = false;
}

Reference JSONTextWriter::build(Draft&& operation, const Effects* foundByCaller)
{
    if (hasStopped())
        return { };
    m_walker.chooseConservativeFields(operation);
    return hasStopped() ? Reference() : m_walker.build(WTF::move(operation), foundByCaller);
}

Reference JSONTextWriter::buildOperator(Opcode opcode, Reference left, Reference right)
{
    if (hasStopped() || !left || !right)
        return { };
    Draft operation = draft(opcode);
    m_walker.chooseConservativeFields(operation);
    operation.setLeftOperand(left);
    operation.setRightOperand(right);
    operation.setForm(formAllowedForTypedOperator(m_walker, operation));
    return m_walker.build(WTF::move(operation));
}

Reference JSONTextWriter::integer(int64_t value)
{
    Draft constant = draft(Opcode::Integer);
    constant.setInteger(value);
    return m_walker.constant(WTF::move(constant));
}

Reference JSONTextWriter::resultAt(Reference results, uint32_t index)
{
    if (!results)
        return { };
    Draft read = draft(Opcode::ResultAt);
    read.setResultsOperand(results);
    read.setIndex(index);
    return build(WTF::move(read));
}

Reference JSONTextWriter::narrowed(Reference value, Reference test, uint32_t operandIndex, bool isOnTakenEdge)
{
    Draft proof = draft(Opcode::Narrowed);
    proof.setValueOperand(value);
    proof.setTestOperand(test);
    proof.setOperandIndex(operandIndex);
    proof.setIsOnTakenEdge(isOnTakenEdge);
    return build(WTF::move(proof));
}

Reference JSONTextWriter::firstNameOf(Reference object)
{
    return object.isValue() ? Reference::value(rootOfCellInSweptCode(m_walker.code(), object.id())) : object;
}

OptionSet<FactOfCell> JSONTextWriter::factsKnownOfCell(Reference object)
{
    return m_walker.state().factsOfCell(object) | m_walker.state().factsOfCell(firstNameOf(object));
}

// ---- Control

BlockID JSONTextWriter::addBlock(BlockFrequency frequency)
{
    BlockID block = m_walker.addBlock(frequency);
    if (m_numberOfOpenLoops)
        m_walker.code().setLoopMarks(block, true, false);
    return block;
}

// The Walker leads an edge to the handler of the block being walked in front of the first operation that may throw, and only out of a block that has no handler edge.
// So a block that is entered while m_handler is set gets its edge here, in front of its first operation.
void JSONTextWriter::standUnderHandler()
{
    if (!m_handler || m_walker.isUnreachable())
        return;
    Code& code = m_walker.code();
    BlockID block = m_walker.currentBlock();
    if (!code.at(block).handler)
        code.closeEdge(code.addHandlerEdge(block), m_handler, { });
}

void JSONTextWriter::continueIn(BlockID block)
{
    m_walker.continueIn(block);
    standUnderHandler();
}

// A jump leaves the walk in its target, which other arms are yet to come to. The target keeps the state that has arrived so far.
void JSONTextWriter::standNowhere()
{
    m_walker.continueIn(BlockID());
}

void JSONTextWriter::takeSlowPath()
{
    m_cursor = { };
    if (hasStopped())
        return;
    m_slowPathIsReached = true;
    m_walker.jumpTo(m_slowPath);
    standNowhere();
}

void JSONTextWriter::requireOrTakeSlowPath(Reference test)
{
    if (hasStopped())
        return;
    if (!test) {
        takeSlowPath();
        return;
    }
    m_slowPathIsReached = true;
    BlockID passing = addBlock();
    m_walker.branch(test, passing, m_slowPath);
    continueIn(passing);
}

AnswerOfJSONTest JSONTextWriter::answerOfTypeTest(Reference value, TypeID tested)
{
    TypeID type = m_walker.typeOf(value);
    if (!type || !tested)
        return AnswerOfJSONTest::No;
    if (types().isSubset(type, tested))
        return AnswerOfJSONTest::Yes;
    return types().overlap(type, tested) ? AnswerOfJSONTest::Unknown : AnswerOfJSONTest::No;
}

AnswerOfJSONTest JSONTextWriter::answerOfComparison(Opcode opcode, Reference left, int64_t right)
{
    Type type = typeOf(left);
    if (!type.isInteger())
        return AnswerOfJSONTest::Unknown;
    Range range = type.integers;
    if (opcode == Opcode::Less) {
        if (range.maximum < right)
            return AnswerOfJSONTest::Yes;
        return range.minimum >= right ? AnswerOfJSONTest::No : AnswerOfJSONTest::Unknown;
    }
    RELEASE_ASSERT(opcode == Opcode::StrictEq);
    if (range.minimum == right && range.maximum == right)
        return AnswerOfJSONTest::Yes;
    return right < range.minimum || right > range.maximum ? AnswerOfJSONTest::No : AnswerOfJSONTest::Unknown;
}

void JSONTextWriter::requireComparison(Opcode opcode, Reference left, int64_t right)
{
    if (hasStopped())
        return;
    switch (answerOfComparison(opcode, left, right)) {
    case AnswerOfJSONTest::Yes:
        return;
    case AnswerOfJSONTest::No:
        takeSlowPath();
        return;
    case AnswerOfJSONTest::Unknown:
        break;
    }
    requireOrTakeSlowPath(buildOperator(opcode, left, integer(right)));
}

void JSONTextWriter::branchOn(ArmOfJSONText& arm, Reference test, BlockFrequency ofOtherValues)
{
    if (!test || hasStopped()) {
        arm.matches = AnswerOfJSONTest::No;
        return;
    }
    BlockID passing = addBlock();
    arm.matches = AnswerOfJSONTest::Unknown;
    arm.otherwise = addBlock(ofOtherValues);
    arm.test = test;
    m_walker.branch(test, passing, arm.otherwise);
    continueIn(passing);
}

ArmOfJSONText JSONTextWriter::armOfTest(Reference test, BlockFrequency ofOtherValues)
{
    ArmOfJSONText arm { AnswerOfJSONTest::No, { }, m_cursor, { }, { }, 0 };
    branchOn(arm, test, ofOtherValues);
    return arm;
}

ArmOfJSONText JSONTextWriter::armOfComparison(Opcode opcode, Reference left, int64_t right)
{
    ArmOfJSONText arm { AnswerOfJSONTest::No, { }, m_cursor, { }, { }, 0 };
    if (hasStopped())
        return arm;
    arm.matches = answerOfComparison(opcode, left, right);
    if (arm.matches == AnswerOfJSONTest::Unknown)
        branchOn(arm, buildOperator(opcode, left, integer(right)), BlockFrequency::Normal);
    return arm;
}

ArmOfJSONText JSONTextWriter::armOfTypeTest(Reference& value, TypeID tested, BlockFrequency ofOtherValues)
{
    ArmOfJSONText arm { AnswerOfJSONTest::No, { }, m_cursor, { }, { }, 0 };
    if (hasStopped())
        return arm;
    arm.matches = answerOfTypeTest(value, tested);
    if (arm.matches != AnswerOfJSONTest::Unknown)
        return arm;
    if (!value.isValue()) {
        arm.matches = AnswerOfJSONTest::No;
        return arm;
    }

    Draft test = draft(Opcode::TypeTest);
    test.setValueOperand(value);
    test.setTestedType(tested);
    arm.tested = value;
    arm.operandIndex = OperandIndex::TypeTest::value;
    branchOn(arm, build(WTF::move(test)), ofOtherValues);
    if (arm.otherwise)
        value = narrowed(value, arm.test, arm.operandIndex, true);
    return arm;
}

ArmOfJSONText JSONTextWriter::armOfShapeTest(Reference& value, LayoutID layout)
{
    ArmOfJSONText arm { AnswerOfJSONTest::No, { }, m_cursor, { }, { }, 0 };
    if (hasStopped())
        return arm;
    arm.matches = answerOfTypeTest(value, types().forLayout(layout));
    if (arm.matches != AnswerOfJSONTest::Unknown)
        return arm;
    if (!value.isValue()) {
        arm.matches = AnswerOfJSONTest::No;
        return arm;
    }

    Draft test = draft(Opcode::ShapeTest);
    test.setObjectOperand(value);
    test.setLayouts(LayoutSets::of(layout));
    arm.tested = value;
    arm.operandIndex = OperandIndex::ShapeTest::object;
    branchOn(arm, build(WTF::move(test)), BlockFrequency::Normal);
    if (arm.otherwise)
        value = narrowed(value, arm.test, arm.operandIndex, true);
    return arm;
}

// The object has these fields and no other, and got them in this order.
ArmOfJSONText JSONTextWriter::armOfKeyOrderTest(Reference object, LayoutID layout, std::span<const uint32_t> created)
{
    ArmOfJSONText arm { AnswerOfJSONTest::No, { }, m_cursor, { }, { }, 0 };
    if (hasStopped())
        return arm;
    // The test compares the word of the cell with the number that the plan has given the order.
    std::optional<uint32_t> asked = m_walker.plan().keyOrders().findOrderWord(layout, created);
    if (!asked)
        return arm;
    std::optional<uint32_t> known = m_walker.state().orderWordOf(object);
    if (!known)
        known = m_walker.state().orderWordOf(firstNameOf(object));
    if (known) {
        arm.matches = *known == *asked ? AnswerOfJSONTest::Yes : AnswerOfJSONTest::No;
        return arm;
    }

    Draft test = draft(Opcode::HasKeyOrder);
    test.setObjectOperand(object);
    test.setLayout(layout);
    test.setFields(created);
    branchOn(arm, build(WTF::move(test)), BlockFrequency::Normal);
    return arm;
}

bool JSONTextWriter::continueWithOtherValues(const ArmOfJSONText& arm)
{
    if (!arm.otherwise || m_walker.hasFailed())
        return false;
    continueIn(arm.otherwise);
    m_cursor = arm.start;
    return !hasStopped();
}

bool JSONTextWriter::continueWithOtherValues(const ArmOfJSONText& arm, Reference& value)
{
    if (!continueWithOtherValues(arm))
        return false;
    if (arm.tested)
        value = narrowed(arm.tested, arm.test, arm.operandIndex, false);
    return !hasStopped();
}

void JSONTextWriter::jumpTo(BlockID merge)
{
    commit();
    if (hasStopped())
        return;
    m_walker.jumpTo(merge);
    standNowhere();
}

void JSONTextWriter::continueAt(BlockID merge)
{
    m_cursor = { };
    if (!m_walker.hasFailed())
        continueIn(merge);
}

// ---- The cursor

void JSONTextWriter::ensureCursor(uint32_t bytes)
{
    if (hasStopped() || (m_cursor.value && m_cursor.reservedBytes >= bytes))
        return;
    RELEASE_ASSERT(bytes <= maxStringBuilderReservation);
    commit();

    uint32_t size = std::max(bytes, leastBytesReservedForJSONText);
    Draft reserve = draft(Opcode::StringBuilderReserve);
    reserve.setBuilderOperand(m_builder);
    reserve.setSize(size);
    m_cursor = { build(WTF::move(reserve)), size };
}

void JSONTextWriter::commit()
{
    Reference cursor = std::exchange(m_cursor, { }).value;
    if (!cursor || hasStopped())
        return;
    Draft commit = draft(Opcode::StringBuilderCommit);
    commit.setBuilderOperand(m_builder);
    commit.setCursorOperand(cursor);
    build(WTF::move(commit));
}

void JSONTextWriter::append(Draft&& cursorOperation)
{
    Opcode opcode = cursorOperation.opcode;
    uint32_t taken = bytesTakenFromReservation(types(), opcode, opcode == Opcode::JSONAppendKey ? cursorOperation.string() : StringID());
    ensureCursor(taken);
    if (hasStopped())
        return;
    cursorOperation.setBuilderOperand(m_builder);
    cursorOperation.setCursorOperand(m_cursor.value);
    Reference cursor = build(WTF::move(cursorOperation));
    m_cursor = { cursor, renewsReservation(opcode) ? maxStringBuilderReservation : m_cursor.reservedBytes - taken };
}

void JSONTextWriter::appendToken(JSONToken token)
{
    Draft operation = draft(Opcode::JSONAppendToken);
    operation.setJsonToken(token);
    append(WTF::move(operation));
}

void JSONTextWriter::appendKeyOf(const PositionOfJSONText& position)
{
    if (position.kind != JSONPosition::Property)
        return;

    StringID text = types().at(position.key).text;
    std::optional<JSONToken> token = position.memberList.separator;
    if (canWriteJSONKeyAtCursor(types(), text)) {
        Draft key = draft(Opcode::JSONAppendKey);
        key.setString(text);
        key.setOpensObject(token == JSONToken::OpenObject);
        key.setFollowsMember(token == JSONToken::Comma);
        append(WTF::move(key));
        return;
    }

    // A name that is long or is not Latin-1 is quoted when the code runs, like any other string.
    if (token)
        appendToken(*token);
    Draft constant = draft(Opcode::String);
    constant.setString(text);
    Draft name = draft(Opcode::JSONAppendString);
    name.setValueOperand(m_walker.constant(WTF::move(constant)));
    append(WTF::move(name));
    appendToken(JSONToken::Colon);
}

void JSONTextWriter::appendCommaOf(const PositionOfJSONText& position)
{
    if (position.endsWithComma())
        appendToken(JSONToken::Comma);
}

// m_cursor names what a JSONAppendValue has given.
void JSONTextWriter::takeResultsOfJSONAppendValue(bool mayBeTurnedAway)
{
    if (hasStopped())
        return;
    Reference results = m_cursor.value;
    m_cursor.value = resultAt(results, cursorResultOfJSONAppendValue);
    if (mayBeTurnedAway)
        requireOrTakeSlowPath(buildOperator(Opcode::StrictEq, resultAt(results, outcomeResultOfJSONAppendValue), integer(static_cast<int64_t>(JSONOutcome::Parsed))));
}

// ---- The text

bool JSONTextWriter::begin(Reference builderOfSurroundingText, uint32_t roomForMembers)
{
    m_membersLeft = roomForMembers;
    if (builderOfSurroundingText) {
        m_builder = builderOfSurroundingText;
        Draft lengthInFront = draft(Opcode::StringBuilderCheckpoint);
        lengthInFront.setBuilderOperand(m_builder);
        m_lengthInFront = build(WTF::move(lengthInFront));
        if (!m_lengthInFront)
            return false;
    } else
        m_builder = build(draft(Opcode::StringBuilderBegin));
    if (!m_builder || hasStopped())
        return false;
    m_slowPath = addBlock(BlockFrequency::Rare);
    return true;
}

void JSONTextWriter::writeRoot(Reference value, const JSONValuePlan& plan, bool underHandler)
{
    if (hasStopped())
        return;
    BlockID handler;
    if (underHandler) {
        Code& code = m_walker.code();
        handler = code.addBlock(BlockRole::CatchHandler, BlockFrequency::Rare);
        code.setLoopMarks(handler, m_walker.isInLoop(), false);
        m_handler = handler;
        m_mayCallSerializers = true;
        // The block that the walk stands in may hold what throws to the handler of the program.
        m_walker.jumpTo(addBlock());
        standUnderHandler();
    }

    appendValue(value, plan, PositionOfJSONText { });
    commit();
    // Finishing a builder throws for a text that is too long. A builder that goes on is asked in its place.
    if (m_lengthInFront) {
        Draft checkLength = draft(Opcode::StringBuilderCheckLength);
        checkLength.setBuilderOperand(m_builder);
        build(WTF::move(checkLength));
    }
    if (!underHandler)
        return;

    // Whatever gives the builder back stands under no handler that gives it back once more.
    m_handler = { };
    m_mayCallSerializers = false;
    BlockID behind = addBlock();
    if (!hasStopped())
        m_walker.jumpTo(behind);
    if (m_walker.hasFailed())
        return;

    // No code of the program has run by the time something is thrown, so the call as written may start from the beginning. It throws what the host throws.
    // No edge of the Walker's leads to the handler, so it brings no state.
    m_walker.continueIn(handler);
    m_walker.state().clear();
    build(draft(Opcode::CatchValue));
    takeSlowPath();
    m_walker.continueIn(behind);
}

void JSONTextWriter::dropText()
{
    Draft drop = draft(m_lengthInFront ? Opcode::StringBuilderRollBack : Opcode::StringBuilderDiscard);
    drop.setBuilderOperand(m_builder);
    if (m_lengthInFront)
        drop.setCheckpointOperand(m_lengthInFront);
    build(WTF::move(drop));
}

bool JSONTextWriter::enterSlowPath()
{
    m_cursor = { };
    if (!m_slowPathIsReached || m_walker.hasFailed())
        return false;
    m_walker.continueIn(m_slowPath);
    if (hasStopped())
        return false;
    dropText();
    return !hasStopped();
}

// ---- Levels

// The host counts an object or a list where it enters it, an empty one too.
void JSONTextWriter::openLevel()
{
    ++m_levelsOpen;
    if (hasStopped())
        return;
    if (!m_levelsAround) {
        if (m_levelsOpen > mostJSONLevels)
            takeSlowPath();
        return;
    }
    requireComparison(Opcode::Less, m_levelsAround, mostJSONLevels - m_levelsOpen + 1);
}

// Every level that is open has passed openLevel(), so the sum is mostJSONLevels at most.
Reference JSONTextWriter::levelsOpenHere()
{
    if (!m_levelsAround)
        return integer(m_levelsOpen);
    return m_levelsOpen ? buildOperator(Opcode::Add, m_levelsAround, integer(m_levelsOpen)) : m_levelsAround;
}

// ---- The serializer of a layout

void JSONTextWriter::returnOutcome(JSONOutcome outcome)
{
    if (hasStopped())
        return;
    Reference returned = integer(static_cast<int64_t>(outcome));
    m_walker.endWithReturn(singleElementSpan(returned), m_origin);
}

// `type T = { v: number; next?: T }`: an object that may hold an object of its own layout is written by a function, which calls itself. Nobody keeps a list of the objects that are being written.
// An object that holds itself has a text without an end, and one of three things ends the writing: the run time is shown the objects that follow once the text has grown long and meets one of them twice,
// as many objects are open as the host allows, or no string is as long as the text. Each drops the text, and the call as written finds the cycle and throws what the host throws.
void JSONTextWriter::writeSerializerOf(LayoutID layout)
{
    // Whoever builds a JSONAppendObject hands over what lies inside these types.
    TypeID typesOfParameters[numberOfSerializerParameters];
    typesOfParameters[static_cast<unsigned>(SerializerParameter::Object)] = types().forLayout(layout);
    typesOfParameters[static_cast<unsigned>(SerializerParameter::Builder)] = types().forOpaque(OpaqueKind::StringBuilder);
    typesOfParameters[static_cast<unsigned>(SerializerParameter::LevelsAround)] = types().integer(Range { 0, mostJSONLevels });
    BlockID entry = m_walker.addBlock();
    Reference parameters[numberOfSerializerParameters];
    for (unsigned index = 0; index < numberOfSerializerParameters; ++index)
        parameters[index] = m_walker.addParameter(entry, typesOfParameters[index], m_walker.plan().conventions().parameterRepresentation(m_walker.program(), m_walker.unit(), index));
    m_walker.code().addEntry(entry);
    m_walker.continueIn(entry);

    Reference object = parameters[static_cast<unsigned>(SerializerParameter::Object)];
    m_builder = parameters[static_cast<unsigned>(SerializerParameter::Builder)];
    m_levelsAround = parameters[static_cast<unsigned>(SerializerParameter::LevelsAround)];
    // Whoever called stands under the handler of the call of JSON.stringify(), or was called by one that does.
    m_mayCallSerializers = true;
    m_buildsLoops = true;
    m_slowPath = addBlock(BlockFrequency::Rare);

    // One compare asks for room on the stack and whether the run time wants to see the object: while it does, the limit lies above every frame.
    Draft hasStackSpace = draft(Opcode::JSONHasStackSpace);
    hasStackSpace.setFailsAtWatchedLevels(true);
    BlockID entered = openMerge();
    ArmOfJSONText hasRoom = armOfTest(build(WTF::move(hasStackSpace)), BlockFrequency::Rare);
    jumpTo(entered);
    if (continueWithOtherValues(hasRoom)) {
        Draft enterWatchedLevel = draft(Opcode::JSONEnterWatchedLevel);
        enterWatchedLevel.setBuilderOperand(m_builder);
        enterWatchedLevel.setHolderOperand(object);
        requireOrTakeSlowPath(build(WTF::move(enterWatchedLevel)));
        jumpTo(entered);
    }
    continueAt(entered);

    if (std::optional<JSONObjectPlan> plan = planJSONObject(m_context, layout)) {
        m_membersLeft = roomForJSONMembersInsideObjectOfSerializer(m_context, *plan);
        appendObjectOfLayout(object, *plan, PositionOfJSONText { });
    } else
        takeSlowPath();
    commit();
    returnOutcome(JSONOutcome::Parsed);

    // Whoever has begun the text drops it.
    if (m_slowPathIsReached && !m_walker.hasFailed()) {
        m_walker.continueIn(m_slowPath);
        returnOutcome(JSONOutcome::Unknown);
    }
}

bool walkSerializerUnit(Walker& walker)
{
    JSONTextWriter(walker, walker.originOfOperation()).writeSerializerOf(walker.unit().layoutOfSerializer());
    return !walker.hasFailed();
}

// ---- The call

// As CallInWalkBuiltins::findCallee(): the type of the callee names one built-in, or the read that has found the callee finds a member that is never replaced. Canonical. Null: nobody can tell.
static BuiltinID builtinCalledInJSONWrites(Walker& walker, const BodyValue& call, const InputsOfTypedOperation& inputs)
{
    TypeArena& types = walker.types();
    TypeID proven = inputs.typeOf(call.calleeOperand());
    if (!proven)
        return { };
    Type type = types.at(proven);
    BuiltinID named = type.isObject() ? singleBuiltinOf(types, type) : BuiltinID();
    if (!named) {
        const BodyIndex& index = walker.currentBodyIndex();
        ValueID read = index.valueBehindCasts(call.calleeOperand());
        if (!read || index.isParameter(read) || index.opcodeOf(read) != Opcode::PropGet || index.value(read).isFreeName())
            return { };
        ValueID objectOfRead = index.value(read).objectOperand();
        PropertyKeyID key = index.value(read).key();
        named = inputs.areOnlyTypes() ? findConstantMemberReadOff(std::as_const(walker), inputs.typeOf(objectOfRead), key) : findConstantMemberReadOff(walker, walker.referenceOf(objectOfRead), key);
    }
    return named ? walker.program().builtins().canonicalID(named) : BuiltinID();
}

// The operand that is written. Null: the call is no call of JSON.stringify, or it hands over a replacer or an indent: a function, a list of names, a number, a text.
static ValueID operandOfJSONStringifyWithoutOptions(Walker& walker, const BodyValue& call, const InputsOfTypedOperation& inputs)
{
    if (call.opcode() != Opcode::Call || call.isDoneInPlace())
        return { };
    std::span<const ValueID> arguments = call.argumentsOperands();
    if (arguments.empty())
        return { };
    if (builtinCalledInJSONWrites(walker, call, inputs) != walker.program().builtins().canonicalID(Builtins::JSONStringify))
        return { };
    // A replacer and an indent that are undefined or null are none.
    bool hasNoOptions = std::ranges::all_of(arguments.subspan(1), [&](ValueID argument) {
        TypeID proven = inputs.typeOf(argument);
        if (!proven)
            return false;
        Type type = walker.types().at(proven);
        return type.isJavaScriptValue() && !type.isNever() && !type.hasObjects() && !(type.primitives & ~Primitive::UndefinedOrNull);
    });
    return hasNoOptions ? arguments[0] : ValueID();
}

namespace {

// What the site writes, gone through as JSONTextWriter goes through it.
struct SiteGoneThroughInJSONWrites {
    // The outermost first.
    Vector<LayoutID, 4> layoutsAround;
    uint32_t membersLeft { 0 };
    bool callsSerializers { false };
};

} // anonymous namespace

// False: some object that the value reaches has neither room at the site nor a serializer, so the text would be dropped at every call, behind work that was for nothing.
static bool siteHasRoomInJSONWrites(const AccessContext& context, const JSONValuePlan& plan, SiteGoneThroughInJSONWrites& site)
{
    switch (plan.kind) {
    case JSONValueKind::Generic:
    case JSONValueKind::Nullish:
        return true;
    case JSONValueKind::Scalar: {
        constexpr bool sitesWriteDates = true;
        return sitesWriteDates || plan.row != Opcode::JSONAppendDate;
    }
    // The run time writes it.
    case JSONValueKind::Array: {
        constexpr bool sitesWriteLists = true;
        return sitesWriteLists;
    }
    case JSONValueKind::ObjectParts:
        return std::ranges::all_of(plan.parts, [&](const JSONValuePlan& part) {
            return siteHasRoomInJSONWrites(context, part, site);
        });
    case JSONValueKind::Objects:
        break;
    }

    for (LayoutID layout : plan.layouts) {
        std::optional<JSONObjectPlan> object = planJSONObject(context, layout);
        if (!object)
            continue;
        uint32_t members = numberOfJSONMembersWrittenOutFor(context, *object);
        if (site.layoutsAround.contains(layout) || members > site.membersLeft || holdsListAmongJSONMembers(context, *object)) {
            constexpr bool sitesCallSerializers = true;
            if (!sitesCallSerializers || !context.plan.conventions().serializerOf(layout))
                return false;
            site.callsSerializers = true;
            continue;
        }

        site.membersLeft -= members;
        site.layoutsAround.append(layout);
        bool hasRoom = std::ranges::all_of(object->orders, [&](const JSONKeyOrderPlan& order) {
            return std::ranges::all_of(order.members, [&](const JSONMemberPlan& member) {
                TypeID proven = provenTypeOfJSONMember(context, layout, member.field);
                if (hasNoJSONText(context, proven))
                    return true;
                return siteHasRoomInJSONWrites(context, planJSONValue(context, proven, writtenTypeOfJSONMember(context, layout, member.field)), site);
            });
        });
        site.layoutsAround.removeLast();
        if (!hasRoom)
            return false;
    }
    return true;
}

std::optional<PlanOfJSONCall> planJSONTextOfCall(Walker& walker, const BodyValue& call, const InputsOfTypedOperation& inputs)
{
    constexpr bool writesJSONTextsByType = true;
    if (!writesJSONTextsByType)
        return std::nullopt;
    // Every test here rejoins, and the closed arm of one is the call as written.
    if (!walker.has(Mechanism::BuiltinCalls) || !walker.has(Mechanism::TypedOperations) || !walker.has(Mechanism::Guesses) || !walker.opensGuesses())
        return std::nullopt;
    PlanOfJSONCall planned;
    planned.source = operandOfJSONStringifyWithoutOptions(walker, call, inputs);
    if (!planned.source)
        return std::nullopt;
    // What runs once gains nothing by code of its own.
    FunctionID function = walker.unit().function;
    bool isInLoop = walker.isInLoop();
    if (!isInLoop && (walker.facts().runsAtMostOnce(function) || walker.program().at(function).isModuleBody()))
        return std::nullopt;

    planned.proven = inputs.typeOf(planned.source);
    if (!planned.proven)
        return std::nullopt;
    if (!inputs.areOnlyTypes()) {
        ExpectationOfValue expectation = findExpectation(walker, planned.source);
        planned.expected = expectation.type;
        planned.expectedIsDeclared = expectation.isDeclared;
    }

    AccessContext context { walker.program(), WalkFacts(walker.facts()), walker.plan(), walker.types(), walker.currentFunction() };
    planned.plan = planJSONValue(context, planned.proven, planned.expected);
    switch (planned.plan.kind) {
    case JSONValueKind::Generic:
    // Of undefined there is no text, and null is four characters.
    case JSONValueKind::Nullish:
        return std::nullopt;
    case JSONValueKind::Scalar:
        // A string builder for one string, number or boolean costs more than JSON.stringify() needs to write it.
        if (planned.plan.row != Opcode::JSONAppendDate)
            return std::nullopt;
        break;
    case JSONValueKind::Array:
    case JSONValueKind::Objects:
    case JSONValueKind::ObjectParts:
        break;
    }
    bool callsSerializersWithAllTheRoom = false;
    if (!writesWholeJSONValue(context, planned.plan, planned.plan.isExpected ? planned.expected : planned.proven, callsSerializersWithAllTheRoom))
        return std::nullopt;

    planned.roomForMembers = isInLoop ? mostJSONMembersWrittenOutInOneUnit : mostJSONMembersWrittenOutOutsideLoops;
    SiteGoneThroughInJSONWrites site { { }, planned.roomForMembers, false };
    if (!siteHasRoomInJSONWrites(context, planned.plan, site))
        return std::nullopt;
    planned.callsSerializers = site.callsSerializers;
    return planned;
}

bool paysToWriteJSONText(Walker& walker, const PlanOfJSONCall& planned)
{
    Reference source = walker.referenceOf(planned.source);
    if (!source || !source.isRecord())
        return false;
    return !planned.plan.isExpected || shouldOpenGuess(walker, source, planned.expected, planned.expectedIsDeclared, closedOperationsOfJSONStringify);
}

bool wouldWriteJSONTextOfCall(Walker& walker, const BodyValue& call, const TypesOfInputs& typesAskedAbout)
{
    std::optional<PlanOfJSONCall> planned = planJSONTextOfCall(walker, call, InputsOfTypedOperation(walker, &typesAskedAbout));
    // writeJSONTextOfCall() is asked of a call that BuiltinCalls has planned.
    return planned && planned->plan.kind != JSONValueKind::Array && wouldCallBuiltin(walker, call, typesAskedAbout) != BuiltinCallAskedAbout::None;
}

bool writeJSONTextOfCall(Walker& walker, const BodyValue& call)
{
    if (walker.isUnreachable())
        return false;
    std::optional<PlanOfJSONCall> planned = planJSONTextOfCall(walker, call, InputsOfTypedOperation(walker, nullptr));
    if (!planned || !paysToWriteJSONText(walker, *planned))
        return false;
    if (writeJSONTextIntoJoinFurtherOn(walker, call, *planned))
        return true;
    // In a body the run time writes a list, as the call as written has it do. Only a text around the list saves something: a string and a copy.
    if (planned->plan.kind == JSONValueKind::Array)
        return false;

    const BodyIndex& index = walker.currentBodyIndex();
    BodyBlock block = index.block(index.blockOf(call.id()));
    uint32_t place = call.id().index() - block.firstValue().index();
    Draft written = walker.draftOf(call);
    if (walker.hasFailed())
        return false;
    Origin origin = written.origin();
    origin.isTailCall = false;

    struct Arm {
        BlockID last;
        Reference value;
    };
    Vector<Arm, 2> arms;

    walker.noteGuessOpened();
    JSONTextWriter writer(walker, origin);
    if (!writer.begin({ }, planned->roomForMembers))
        return true;
    writer.writeRoot(walker.referenceOf(planned->source), planned->plan, planned->callsSerializers);
    if (!walker.isUnreachable() && !walker.hasFailed()) {
        Draft finish(Opcode::StringBuilderFinish, origin);
        walker.chooseConservativeFields(finish);
        finish.setBuilderOperand(writer.builder());
        finish.expected = written.expected;
        finish.inputValueGiven = call.id();
        if (Reference text = walker.build(WTF::move(finish)))
            arms.append(Arm { walker.currentBlock(), text });
    }
    if (walker.hasFailed())
        return true;

    if (writer.enterSlowPath()) {
        Walker::ClosedArm closedArm(walker);
        walker.walkStretch(block.id(), place, place + 1);
        if (!walker.isUnreachable() && !walker.hasFailed())
            arms.append(Arm { walker.currentBlock(), walker.referenceOf(call.id()) });
    }
    if (walker.hasFailed() || arms.isEmpty())
        return true;

    if (arms.size() == 1 || !arms[0].value || !arms[1].value) {
        walker.continueIn(arms[0].last);
        walker.bind(call.id(), arms[0].value);
        if (arms.size() == 1)
            return true;
        walker.fail("the call of JSON.stringify as written gives nothing"_s);
        return true;
    }

    const Code& code = walker.code();
    TypeID type = joinOf(walker.types(), walker.facts().families(), code.at(arms[0].value.id()).type, code.at(arms[1].value.id()).type);
    Representation forms[] { walker.representationOf(arms[0].value), walker.representationOf(arms[1].value) };
    BlockID meeting = walker.addBlock();
    Reference met = walker.addParameter(meeting, type, walker.formOfParameter(type, forms));
    for (const Arm& arm : arms) {
        walker.continueIn(arm.last);
        walker.jumpTo(meeting, singleElementSpan(arm.value));
        if (walker.hasFailed())
            return true;
    }
    walker.continueIn(meeting);
    walker.bind(call.id(), met);
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
