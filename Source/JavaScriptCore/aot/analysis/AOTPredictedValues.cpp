/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPredictedValues.h"

#if ENABLE(AOT)

#include "AOTCFG.h"
#include "AOTEdgeResults.h"
#include "AOTTypeArena.h"
#include "AOTValueIdentity.h"
#include "AOTValueSubstitution.h"

namespace JSC { namespace AOT { namespace Typed {


namespace {

// Where no edge leads back, whatever a value takes a mark from stands in front of it in reverse postorder, so one walk finds everything.
struct MarkWalk {
    const Function& function;
    const CFG& cfg;
    Vector<uint8_t>& marks;
    uint8_t marksOfWhatIsMade;
    uint8_t marksOfSameValue;
    uint8_t markOfTestedValue;
    const Program* programToReadComparisonsWith;
    bool hasChanged { false };

    uint8_t at(ValueID value) const { return value ? marks[value.index()] : 0; }

    void add(ValueID value, uint8_t added)
    {
        uint8_t& held = marks[value.index()];
        hasChanged |= added & ~held;
        held |= added;
    }

    uint8_t marksOfOperands(const Instruction& user) const
    {
        uint8_t found = 0;
        for (ValueID operand : user.operands())
            found |= at(operand);
        return found;
    }

    bool takesTypeFromTest(const EdgeResult& result) const
    {
        const Value& test = function.at(result.test);
        if (at(result.subject) & marksOfWhatIsMade || provesOf(test.opcode) != Proof::IntegerBound)
            return true;
        return programToReadComparisonsWith->typeOf(function, test.operand(result.operandIndex ? 0 : 1)).integers != Range::full();
    }

    uint8_t marksOnEdge(Edge edge, uint32_t parameterIndex) const
    {
        IncomingValue incoming = incomingValueOf(function, edge, parameterIndex);
        if (incoming.kind == IncomingValue::Kind::Argument)
            return at(incoming.argument);
        EdgeResult result = edgeResultAt(function, incoming.result);
        uint8_t found = at(result.subject) & marksOfSameValue;
        if (result.kind == EdgeResultKind::NarrowedOperand)
            found |= markOfTestedValue;
        if (!programToReadComparisonsWith)
            return found | (marksOfOperands(function.at(edge.from).terminator) & marksOfWhatIsMade);
        found |= at(result.subject) & marksOfWhatIsMade;
        if (result.test && takesTypeFromTest(result))
            found |= at(result.test) & marksOfWhatIsMade;
        return found;
    }

    void run(NOESCAPE const Invocable<uint8_t(ValueID, const Value&)> auto& ownMarksOf)
    {
        do {
            hasChanged = false;
            for (BlockID id : cfg.reversePostOrder()) {
                const Block& block = function.at(id);
                for (uint32_t index = 0; id != entryBlock && index < block.parameters.size(); ++index) {
                    for (Edge edge : cfg.incomingEdges(id))
                        add(block.parameters[index], marksOnEdge(edge, index));
                }
                for (ValueID id : block.values) {
                    const Value& value = function.at(id);
                    uint8_t found = ownMarksOf(id, value) | (marksOfOperands(value) & marksOfWhatIsMade);
                    if (isCast(value.opcode))
                        found |= at(value.operand(0)) & marksOfSameValue;
                    add(id, found);
                }
            }
        } while (hasChanged && cfg.hasCycle());
    }
};

} // anonymous namespace

static BitVector walkFrom(const Function& function, const CFG& cfg, std::span<const ValueID> values, uint8_t marksOfWhatIsMade, uint8_t marksOfSameValue)
{
    Vector<uint8_t> marks(FillWith { }, function.values.size(), static_cast<uint8_t>(0));
    for (ValueID value : values)
        marks[value.index()] = 1;
    MarkWalk { function, cfg, marks, marksOfWhatIsMade, marksOfSameValue, 0, nullptr }.run([](ValueID, const Value&) -> uint8_t { return 0; });
    BitVector isMarked(function.values.size());
    for (size_t index = 0; index < marks.size(); ++index) {
        if (marks[index])
            isMarked.quickSet(index);
    }
    return isMarked;
}

BitVector valuesBehind(const Function& function, const CFG& cfg, std::span<const ValueID> values)
{
    return walkFrom(function, cfg, values, 1, 0);
}

BitVector valuesThatAre(const Function& function, const CFG& cfg, std::span<const ValueID> values)
{
    return walkFrom(function, cfg, values, 0, 1);
}

PredictedValues::PredictedValues(const Function& function, const CFG& cfg)
    : PredictedValues(function, cfg, Options { })
{
}

PredictedValues::PredictedValues(const Function& function, const CFG& cfg, Options options)
    : m_numberOfValues(function.values.size())
{
    bool hasPredictions = false;
    for (const Value& value : function.values) {
        m_hasChecks |= value.opcode == Opcode::Check;
        hasPredictions |= value.opcode == Opcode::Check || value.opcode == Opcode::Expect;
    }
    if (!hasPredictions)
        return;
    m_marks = Vector<uint8_t>(FillWith { }, function.values.size(), static_cast<uint8_t>(0));
    MarkWalk { function, cfg, m_marks, marksOfWhatIsMade, UncheckedItself, BehindTest, options.programToReadComparisonsWith }.run([&](ValueID id, const Value& value) -> uint8_t {
        if (value.opcode == Opcode::Check)
            return options.checksToLeaveOut && options.checksToLeaveOut->get(id.index()) ? 0 : BehindCheck;
        if (value.opcode != Opcode::Expect)
            return 0;
        return value.isDeclared() ? BehindExpectation | UncheckedItself : BehindExpectation;
    });
}

BitVector PredictedValues::valuesBehindChecks() const
{
    BitVector isBehindCheck(m_numberOfValues);
    for (size_t index = 0; index < m_marks.size(); ++index) {
        if (m_marks[index] & BehindCheck)
            isBehindCheck.quickSet(index);
    }
    return isBehindCheck;
}

ProvenType Proven::byRules(const PredictedValues& predicted, ValueID value, TypeID type)
{
    return { type, predicted.isBehindCheck(value) ? ProofScope::BehindChecks : ProofScope::EveryVersion };
}

ProvenType Proven::asClaimedBy(const Value& assumeType)
{
    RELEASE_ASSERT(assumeType.opcode == Opcode::AssumeType);
    return { assumeType.provenType(), assumeType.isBehindChecks() ? ProofScope::BehindChecks : ProofScope::EveryVersion };
}

void settleProofsBehindAnsweredChecks(Function& function, const CFG& cfg)
{
    std::optional<PredictedValues> predicted;
    for (uint32_t index = 0; index < function.values.size(); ++index) {
        const Value& value = function.values[index];
        if (value.opcode != Opcode::AssumeType || !value.isBehindChecks())
            continue;
        if (!predicted)
            predicted.emplace(function, cfg);
        if (!predicted->isBehindCheck(value.valueOperand()))
            function.edit(ValueID(index), ValueEdit::Definition).setIsBehindChecks(false);
    }
}

void becomeExpect(Value& value, ValueID operand, ExpectedType expected, InPlaceOfCheck inPlaceOfCheck)
{
    value.become(Opcode::Expect);
    value.setValueOperand(operand);
    value.setExpectedType(expected);
    value.setIsDeclared(inPlaceOfCheck == InPlaceOfCheck::Yes);
}

static LayoutSetID withReceiversNobodyHasSeen(TypeArena& types, LayoutSetID stamp)
{
    const LayoutSet& receivers = types.at(stamp);
    if (!receivers.isComplete)
        return stamp;
    if (receivers.members.isEmpty() || receivers.hasMembersWithoutLayout)
        return { };
    return types.intern(LayoutSet { .members = receivers.members, .isComplete = false });
}

static CalleeSetID withCalleesNobodyHasSeen(TypeArena& types, CalleeSetID stamp)
{
    const CalleeSet& callees = types.at(stamp);
    if (!callees.isComplete)
        return stamp;
    return types.intern(CalleeSet { .functions = callees.functions, .builtins = callees.builtins, .isComplete = false });
}

ForgottenProofs forgetProofsBehind(Function& function, const CFG& cfg, TypeArena& types, std::span<const ValueID> values, const BitVector& checksThatGo, ExpectedTypes expectedTypes)
{
    if (values.empty())
        return { };
    return forgetProofsOf(function, types, valuesBehind(function, cfg, values), checksThatGo, expectedTypes);
}

ForgottenProofs forgetProofsOf(Function& function, TypeArena& types, const BitVector& isBehind, const BitVector& checksThatGo, ExpectedTypes expectedTypes)
{
    ForgottenProofs forgotten;
    ExpectedType nothing { types.withPrimitives(TypeTable::anything, Primitive::Absent) };
    auto expect = [&](ValueID id, TypeID type) {
        Value& value = function.edit(id);
        InPlaceOfCheck inPlaceOfCheck = value.opcode == Opcode::Check ? InPlaceOfCheck::Yes : InPlaceOfCheck::No;
        if (expectedTypes == ExpectedTypes::Dropped)
            return becomeExpect(value, value.valueOperand(), nothing, inPlaceOfCheck);
        ExpectedType expected { types.intern(types.at(type).withoutClassProofs()) };
        becomeExpect(value, value.valueOperand(), expected, inPlaceOfCheck);
        value.setExpected(expected);
    };
    auto takesValueBehind = [&](const Instruction& user) {
        return std::ranges::any_of(user.operands(), [&](ValueID operand) { return operand && isBehind.get(operand.index()); });
    };

    for (size_t index : isBehind) {
        ValueID id(index);
        const Value& value = function.at(id);
        if (value.opcode == Opcode::ValidateType || value.opcode == Opcode::ValidateCallee) {
            becomeUndefined(function, id);
            forgotten.hasErasedValidation = true;
        } else if (value.opcode == Opcode::Check) {
            if (checksThatGo.get(index))
                expect(id, value.expectedType().predicted());
        } else if (value.opcode == Opcode::AssumeType) {
            if (!value.isBehindChecks())
                continue;
            forgotten.claims.append({ value.valueOperand(), value.provenType() });
            expect(id, value.provenType());
        } else if (value.opcode == Opcode::Upcast) {
            if (expectedTypes == ExpectedTypes::Kept)
                expect(id, value.castType());
            else
                function.edit(id).setCastType(nothing.predicted());
        }
    }

    for (uint32_t index = 0; index < function.blocks.size(); ++index) {
        const Block& block = function.blocks[index];
        const Terminator& terminator = block.terminator;
        if (terminator.kind == TerminatorKind::ReturnCall && isBehind.get(terminator.callOperand().index()) && terminator.casts().size())
            function.edit(BlockID(index), BlockEdit::Terminator).terminator.setCasts({ });
        for (ValueID id : block.values) {
            const Value& value = function.at(id);
            bool hasReceivers = Value::opcodesWithReceivers.contains(value.opcode) && value.receivers();
            bool hasCallees = Value::opcodesWithCallees.contains(value.opcode) && value.callees();
            if ((!hasReceivers && !hasCallees) || !takesValueBehind(value))
                continue;
            // Where the type of the check is expected no longer, what flow has seen is what is left to expect.
            if (hasReceivers)
                function.edit(id, ValueEdit::Definition).setReceivers(expectedTypes == ExpectedTypes::Dropped ? withReceiversNobodyHasSeen(types, value.receivers()) : LayoutSetID());
            if (hasCallees && withCalleesNobodyHasSeen(types, value.callees()) != value.callees())
                function.edit(id, ValueEdit::Definition).setCallees(withCalleesNobodyHasSeen(types, value.callees()));
        }
    }
    return forgotten;
}

ForgottenProofs expectInsteadOfChecking(Function& function, const CFG& cfg, TypeArena& types, std::span<const ValueID> checks, ExpectedTypes expectedTypes)
{
    BitVector checksThatGo(function.values.size());
    for (ValueID check : checks) {
        RELEASE_ASSERT(function.at(check).opcode == Opcode::Check);
        checksThatGo.quickSet(check.index());
    }
    return forgetProofsBehind(function, cfg, types, checks, checksThatGo, expectedTypes);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
