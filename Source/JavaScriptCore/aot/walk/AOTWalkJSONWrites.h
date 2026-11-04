/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTWalk.h"
#include "AOTWalkJSONWritePlan.h"
#include <optional>
#include <span>

namespace JSC { namespace AOT { namespace Typed {

// `JSON.stringify(value)` where the types say what the value reaches: the text is written by compiled code, member by member, into a string builder.
// Nothing that is written so runs code of the program, and nobody sees a builder. So wherever a value turns out not to be what its plan says, the text is dropped and the call as written starts from the beginning.

// ---- AOTWalkJSONWrites.cpp
// For buildCallOfBuiltinForTexts(), of a call that BuiltinCalls has planned. True: the text is written and the call is bound. False: nothing was built.
bool writeJSONTextOfCall(Walker&, const BodyValue& call);
// Its twin for Guesses and the looks: with operands that are proven to have these types the text is written. It only answers.
bool wouldWriteJSONTextOfCall(Walker&, const BodyValue& call, const TypesOfInputs&);
// Walker::walk(), of a unit with CodeRole::Serializer: Code is reset and holds no block. False after a failure of the walk.
bool walkSerializerUnit(Walker&);

// ---- AOTWalkJSONWriteTexts.cpp
// `Response.json(row)`. True: the built-in is handed the text that compiled code has written, and the call is bound. False: nothing was built.
bool handJSONTextToBuiltin(Walker&, const BodyValue& call);

// ---- AOTWalkJSONWriteLists.cpp. The loop over the elements of a list, over Code's own doors. No body of the input holds it, so nobody has looked at it ahead: the types of the head's parameters are said,
// and what enters and what goes round has to lie within them. No cursor goes round. Only in a serializer unit: the head is entered with the empty state, and nothing leaves the loop.
// Whoever makes a block while the loop is open marks it as one of the loop.
class BuiltLoopOfSerializers {
    WTF_MAKE_NONCOPYABLE(BuiltLoopOfSerializers);
public:
    // The walk stands in the head behind it.
    BuiltLoopOfSerializers(Walker&, std::span<const Reference> entering, std::span<const TypeID> typesOfParameters);
    Reference parameter(uint32_t index) const { return m_parameters[index]; }
    // Ends the current block with the edge that comes round.
    void goRound(std::span<const Reference> values);
    // No more edges come round, and the walk stands nowhere.
    void close();

private:
    bool leadToHead(std::span<const Reference> values);

    Walker& m_walker;
    BlockID m_head;
    Vector<Reference, 2> m_parameters;
    ValueID m_poll;
    bool m_standsInLoopAround { false };
    bool m_hasBackEdge { false };
};

// ---- From one file of the package to another, and to nobody else.

struct PlanOfJSONCall {
    // The operand that is written, a value of the body.
    ValueID source;
    TypeID proven;
    // Null: nothing is expected, or only types were asked about.
    TypeID expected;
    bool expectedIsDeclared { false };
    JSONValuePlan plan;
    // How many members the site writes out. An object for whose members there is no room is written by the serializer of its layout.
    uint32_t roomForMembers { 0 };
    bool callsSerializers { false };
};
// AOTWalkJSONWrites.cpp. Every refusal that goes by the call, the types and the place. It builds nothing. Nothing: the call goes on as if nobody had asked.
std::optional<PlanOfJSONCall> planJSONTextOfCall(Walker&, const BodyValue& call, const InputsOfTypedOperation&);
// A test of an expected type stands only where the one predicate of Guesses says so.
bool paysToWriteJSONText(Walker&, const PlanOfJSONCall&);
// AOTWalkJSONWriteTexts.cpp. Of a call that is planned and pays. `out += JSON.stringify(row) + "\n"`, `"data: " + JSON.stringify(e) + "\n\n"`: a row of joins further on in the stretch alone takes the text, and nothing that anybody sees stands between.
// The walk goes on up to the last join of the row, and the text is written there, between the other parts. True: the row is built and bound, and walked. False: nothing was built.
bool writeJSONTextIntoJoinFurtherOn(Walker&, const BodyValue& call, const PlanOfJSONCall&);

struct CursorOfJSONText {
    Reference value;
    uint32_t reservedBytes { 0 };
};

// Where the next member starts, the text of the object is empty, ends in a member, or ends in "{" or ",".
struct MemberListOfJSONText {
    std::optional<JSONToken> separator;
    JSONToken closing;
};

struct PositionOfJSONText {
    // A member that starts after a separator writes the separator of the next member.
    bool endsWithComma() const { return kind == JSONPosition::Property && !memberList.separator; }

    JSONPosition kind { JSONPosition::Root };
    PropertyKeyID key;
    MemberListOfJSONText memberList { std::nullopt, JSONToken::CloseObjectOverComma };
};

enum class AnswerOfJSONTest : uint8_t { No, Yes, Unknown };

struct ArmOfJSONText {
    AnswerOfJSONTest matches { AnswerOfJSONTest::No };
    // Where the values go on that the test has turned away: this edge alone enters it. Null: the test is decided.
    BlockID otherwise;
    CursorOfJSONText start;
    Reference test;
    // What the test has looked at, as its operand number `operandIndex`. Null: it narrows nothing.
    Reference tested;
    uint32_t operandIndex { 0 };
};

class JSONTextWriter {
    WTF_MAKE_NONCOPYABLE(JSONTextWriter);
public:
    JSONTextWriter(Walker&, Origin);

    Reference builder() const { return m_builder; }
    // Null: the text gets a builder of its own. Otherwise it is written behind what that builder holds. False: no way leads on.
    bool begin(Reference builderOfSurroundingText, uint32_t roomForMembers);
    // `underHandler`: the text is written under a handler that drops it and whatever was thrown. Without one no serializer is called: what a serializer throws over an object that holds itself is not what the host throws.
    void writeRoot(Reference value, const JSONValuePlan&, bool underHandler);
    // The walk stands in the one block that every failed test leads to, behind what gives the builder back to the pool, or puts it back to what it held in front of the text. False: nothing leads there.
    bool enterSlowPath();
    // All of a unit with the role Serializer.
    void writeSerializerOf(LayoutID);

private:
    static constexpr MemberListOfJSONText notOpened { JSONToken::OpenObject, JSONToken::EmptyObject };
    static constexpr MemberListOfJSONText afterMember { JSONToken::Comma, JSONToken::CloseObject };
    static constexpr MemberListOfJSONText afterSeparator { std::nullopt, JSONToken::CloseObjectOverComma };

    // An object or a list, from the tests in front of its first byte to its last byte.
    class LevelScope {
        WTF_MAKE_NONCOPYABLE(LevelScope);
    public:
        explicit LevelScope(JSONTextWriter& writer)
            : m_writer(writer)
        {
            writer.openLevel();
        }
        ~LevelScope() { --m_writer.m_levelsOpen; }

    private:
        JSONTextWriter& m_writer;
    };

    bool hasStopped() const { return m_walker.isUnreachable() || m_walker.hasFailed(); }
    TypeArena& types() { return m_walker.types(); }
    Type typeOf(Reference reference) { return types().at(m_walker.typeOf(reference)); }

    // ---- AOTWalkJSONWrites.cpp: operations, control, the cursor
    Draft draft(Opcode opcode) { return Draft(opcode, m_origin); }
    // Of a draft whose fields are set.
    Reference build(Draft&&, const Effects* foundByCaller = nullptr);
    Reference buildOperator(Opcode, Reference left, Reference right);
    Reference integer(int64_t);
    Reference resultAt(Reference results, uint32_t index);
    // At the head of a block that the one edge of the branch on the test enters.
    Reference narrowed(Reference value, Reference test, uint32_t operandIndex, bool isOnTakenEdge);
    // The state may know the cell under the name that it was made or has arrived under.
    Reference firstNameOf(Reference object);
    OptionSet<FactOfCell> factsKnownOfCell(Reference object);

    BlockID addBlock(BlockFrequency = BlockFrequency::Normal);
    void standUnderHandler();
    void continueIn(BlockID);
    void standNowhere();
    void takeSlowPath();
    void requireOrTakeSlowPath(Reference test);
    void requireComparison(Opcode, Reference left, int64_t right);
    AnswerOfJSONTest answerOfTypeTest(Reference value, TypeID tested);
    AnswerOfJSONTest answerOfComparison(Opcode, Reference left, int64_t right);
    // The walk goes on where the test has passed.
    void branchOn(ArmOfJSONText&, Reference test, BlockFrequency ofOtherValues);
    ArmOfJSONText armOfTest(Reference test, BlockFrequency ofOtherValues = BlockFrequency::Normal);
    ArmOfJSONText armOfComparison(Opcode, Reference left, int64_t right);
    // `value` comes back under what the test has shown of it.
    ArmOfJSONText armOfTypeTest(Reference& value, TypeID tested, BlockFrequency ofOtherValues = BlockFrequency::Normal);
    ArmOfJSONText armOfShapeTest(Reference& value, LayoutID);
    ArmOfJSONText armOfKeyOrderTest(Reference object, LayoutID, std::span<const uint32_t> created);
    // Behind the arm, which has jumped on. False: no value comes by here.
    bool continueWithOtherValues(const ArmOfJSONText&);
    bool continueWithOtherValues(const ArmOfJSONText&, Reference& value);
    // Where arms meet. No cursor goes through a parameter: each arm commits in front of the jump, and whoever writes on reserves anew.
    BlockID openMerge() { return addBlock(); }
    void jumpTo(BlockID merge);
    void continueAt(BlockID merge);

    void ensureCursor(uint32_t bytes);
    void commit();
    void append(Draft&& cursorOperation);
    void appendToken(JSONToken);
    void appendKeyOf(const PositionOfJSONText&);
    void appendCommaOf(const PositionOfJSONText&);
    void takeResultsOfJSONAppendValue(bool mayBeTurnedAway);

    void dropText();
    void openLevel();
    Reference levelsOpenHere();
    void returnOutcome(JSONOutcome);

    // ---- AOTWalkJSONWriteValues.cpp
    void appendValue(Reference value, const JSONValuePlan&, const PositionOfJSONText&);
    void appendPlannedValue(Reference value, const JSONValuePlan&, const PositionOfJSONText&);
    void appendTestedValue(Reference value, const JSONValuePlan&, const PositionOfJSONText&);
    void appendGeneric(Reference value, const PositionOfJSONText&);
    void requirePristine(Reference value, BuiltinClassID, PristineAspectSet);
    bool firstReadIsSeen(BuiltinID location);
    void buildFirstReadOf(BuiltinID location);
    void appendFirstReadsOfMembersOfDate(Reference time);
    void appendDate(Reference date, const PositionOfJSONText&);
    void appendObjectParts(Reference value, const JSONValuePlan&, const PositionOfJSONText&);
    void appendObjects(Reference value, const JSONValuePlan&, const PositionOfJSONText&);
    void lookAtCell(Reference object, const JSONObjectPlan&);
    void appendObjectOfLayout(Reference object, const JSONObjectPlan&, const PositionOfJSONText&);
    void appendObjectByCall(Reference object, LayoutID, const PositionOfJSONText&);
    void appendMember(Reference object, LayoutID, const JSONMemberPlan&, bool isFilled, MemberListOfJSONText&);

    // ---- AOTWalkJSONWriteLists.cpp
    void appendArray(Reference array, const JSONValuePlan&, const PositionOfJSONText&);
    void appendListByRunTime(Reference array, Reference levelsAround, const PositionOfJSONText&);
    void appendArrayElements(Reference array, Reference length, ArrayStorage, TypeID writtenElementType);
    void appendArrayElement(Reference array, Reference index, ArrayStorage, TypeID writtenElementType);

    Walker& m_walker;
    AccessContext m_context;
    Origin m_origin;
    Reference m_builder;
    // What the builder of a surrounding text held in front of this one. Null: the builder is the text's own.
    Reference m_lengthInFront;
    CursorOfJSONText m_cursor;
    BlockID m_slowPath;
    bool m_slowPathIsReached { false };
    // Blocks that are entered stand under this handler block. Null: under the handler of the block being walked.
    BlockID m_handler;
    // The outermost first. An object of one of them is not written out inside itself: the serializer of its layout writes it.
    Vector<LayoutID, 4> m_layoutsAround;
    // The outermost first. From m_firstListDirectlyAround on no object lies between them: repeatsJSONListAround().
    Vector<JSONListAround, 4> m_listsAround;
    uint32_t m_firstListDirectlyAround { 0 };
    uint32_t m_membersLeft { 0 };
    bool m_mayCallSerializers { false };
    // A serializer unit. In a body the run time writes a list.
    bool m_buildsLoops { false };
    uint32_t m_numberOfOpenLoops { 0 };
    // Of a serializer: how many objects and lists its callers have open. Null: none.
    Reference m_levelsAround;
    // How many this unit has open where the walk stands.
    uint32_t m_levelsOpen { 0 };
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
