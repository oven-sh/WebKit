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

bool writeJSONTextOfCall(Walker&, const BodyValue& call);
bool wouldWriteJSONTextOfCall(Walker&, const BodyValue& call, const TypesOfInputs&);
bool walkSerializerUnit(Walker&);

class BuiltLoopOfSerializers {
    WTF_MAKE_NONCOPYABLE(BuiltLoopOfSerializers);
public:
    BuiltLoopOfSerializers(Walker&, std::span<const Reference> entering, std::span<const TypeID> typesOfParameters);
    Reference parameter(uint32_t index) const { return m_parameters[index]; }
    void goRound(std::span<const Reference> values);
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

struct PlanOfJSONCall {
    ValueID source;
    TypeID proven;
    TypeID expected;
    bool expectedIsDeclared { false };
    JSONValuePlan plan;
    uint32_t roomForMembers { 0 };
    bool callsSerializers { false };
};
std::optional<PlanOfJSONCall> planJSONTextOfCall(Walker&, const BodyValue& call, const InputsOfTypedOperation&);
bool paysToWriteJSONText(Walker&, const PlanOfJSONCall&);
bool writeJSONTextIntoJoinFurtherOn(Walker&, const BodyValue& call, const PlanOfJSONCall&);

struct CursorOfJSONText {
    Reference value;
    uint32_t reservedBytes { 0 };
};

struct MemberListOfJSONText {
    std::optional<JSONToken> separator;
    JSONToken closing;
};

struct PositionOfJSONText {
    bool endsWithComma() const { return kind == JSONPosition::Property && !memberList.separator; }

    JSONPosition kind { JSONPosition::Root };
    PropertyKeyID key;
    MemberListOfJSONText memberList { std::nullopt, JSONToken::CloseObjectOverComma };
};

enum class AnswerOfJSONTest : uint8_t { No, Yes, Unknown };

struct ArmOfJSONText {
    AnswerOfJSONTest matches { AnswerOfJSONTest::No };
    BlockID otherwise;
    CursorOfJSONText start;
    Reference test;
    Reference tested;
    uint32_t operandIndex { 0 };
};

class JSONTextWriter {
    WTF_MAKE_NONCOPYABLE(JSONTextWriter);
public:
    JSONTextWriter(Walker&, Origin);

    Reference builder() const { return m_builder; }
    bool begin(Reference builderOfSurroundingText, uint32_t roomForMembers);
    void writeRoot(Reference value, const JSONValuePlan&, bool underHandler);
    bool enterSlowPath();
    void writeSerializerOf(LayoutID);

private:
    static constexpr MemberListOfJSONText notOpened { JSONToken::OpenObject, JSONToken::EmptyObject };
    static constexpr MemberListOfJSONText afterMember { JSONToken::Comma, JSONToken::CloseObject };
    static constexpr MemberListOfJSONText afterSeparator { std::nullopt, JSONToken::CloseObjectOverComma };

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

    Draft draft(Opcode opcode) { return Draft(opcode, m_origin); }
    Reference build(Draft&&, const Effects* foundByCaller = nullptr);
    Reference buildOperator(Opcode, Reference left, Reference right);
    Reference integer(int64_t);
    Reference resultAt(Reference results, uint32_t index);
    Reference narrowed(Reference value, Reference test, uint32_t operandIndex, bool isOnTakenEdge);
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
    void branchOn(ArmOfJSONText&, Reference test, BlockFrequency ofOtherValues);
    ArmOfJSONText armOfTest(Reference test, BlockFrequency ofOtherValues = BlockFrequency::Normal);
    ArmOfJSONText armOfComparison(Opcode, Reference left, int64_t right);
    ArmOfJSONText armOfTypeTest(Reference& value, TypeID tested, BlockFrequency ofOtherValues = BlockFrequency::Normal);
    ArmOfJSONText armOfShapeTest(Reference& value, LayoutID);
    ArmOfJSONText armOfKeyOrderTest(Reference object, LayoutID, std::span<const uint32_t> created);
    bool continueWithOtherValues(const ArmOfJSONText&);
    bool continueWithOtherValues(const ArmOfJSONText&, Reference& value);
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

    void appendValue(Reference value, const JSONValuePlan&, const PositionOfJSONText&);
    void appendPlannedValue(Reference value, const JSONValuePlan&, const PositionOfJSONText&);
    void appendTestedValue(Reference value, const JSONValuePlan&, const PositionOfJSONText&);
    void appendGeneric(Reference value, const PositionOfJSONText&);
    void requirePristine(Reference value, BuiltinClassID, PristineAspectSet);
    void appendObjectParts(Reference value, const JSONValuePlan&, const PositionOfJSONText&);
    void appendObjects(Reference value, const JSONValuePlan&, const PositionOfJSONText&);
    void lookAtCell(Reference object, const JSONObjectPlan&);
    void appendObjectOfLayout(Reference object, const JSONObjectPlan&, const PositionOfJSONText&);
    void appendObjectByCall(Reference object, LayoutID, const PositionOfJSONText&);
    void appendMember(Reference object, LayoutID, const JSONMemberPlan&, bool isFilled, MemberListOfJSONText&);

    void appendArray(Reference array, const JSONValuePlan&, const PositionOfJSONText&);
    void appendListByRunTime(Reference array, Reference levelsAround, const PositionOfJSONText&);
    void appendArrayElements(Reference array, Reference length, ArrayStorage, TypeID writtenElementType);
    void appendArrayElement(Reference array, Reference index, ArrayStorage, TypeID writtenElementType);

    Walker& m_walker;
    AccessContext m_context;
    Origin m_origin;
    Reference m_builder;
    Reference m_lengthInFront;
    CursorOfJSONText m_cursor;
    BlockID m_slowPath;
    bool m_slowPathIsReached { false };
    BlockID m_handler;
    Vector<LayoutID, 4> m_layoutsAround;
    Vector<JSONListAround, 4> m_listsAround;
    uint32_t m_firstListDirectlyAround { 0 };
    uint32_t m_membersLeft { 0 };
    bool m_mayCallSerializers { false };
    bool m_buildsLoops { false };
    uint32_t m_numberOfOpenLoops { 0 };
    Reference m_levelsAround;
    uint32_t m_levelsOpen { 0 };
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
