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
#include <wtf/HashSet.h>

namespace JSC { namespace AOT { namespace Typed {


namespace {

constexpr OpcodeSet casts = [] {
    OpcodeSet opcodes;
    for (Opcode opcode : allMembersOf<Opcode>) {
        if (isCast(opcode))
            opcodes.add(opcode);
    }
    return opcodes;
}();

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

    uint8_t marksOfResult(const EdgeResult& result) const
    {
        uint8_t found = at(result.subject) & marksOfSameValue;
        if (!programToReadComparisonsWith)
            return found;
        if (result.kind == EdgeResultKind::NarrowedOperand)
            found |= markOfTestedValue;
        found |= at(result.subject) & marksOfWhatIsMade;
        if (result.test && takesTypeFromTest(result))
            found |= at(result.test) & marksOfWhatIsMade;
        return found;
    }

    void handOnMarks(Edge edge, const Target& target)
    {
        const Block& block = function.at(target.block);
        uint32_t numberOfArguments = static_cast<uint32_t>(target.arguments.size());
        for (uint32_t index = 0; index < numberOfArguments; ++index) {
            if (uint8_t found = at(target.arguments[index]))
                add(block.parameters[index], found);
        }
        if (numberOfArguments == block.parameters.size())
            return;

        uint8_t marksOfTerminator = programToReadComparisonsWith ? 0 : marksOfOperands(function.at(edge.from).terminator) & marksOfWhatIsMade;
        bool readsResults = programToReadComparisonsWith || marksOfSameValue;
        for (uint32_t index = numberOfArguments; index < block.parameters.size(); ++index) {
            uint8_t found = marksOfTerminator;
            if (readsResults)
                found |= marksOfResult(edgeResultAt(function, { edge.from, edge.targetIndex, index - numberOfArguments }));
            if (found)
                add(block.parameters[index], found);
        }
    }

    // Nothing takes a mark in front of the first value that has one. A test that is read gives one to what it leaves.
    uint32_t findFirstBlockWithMark(NOESCAPE const Invocable<uint8_t(ValueID, const Value&)> auto& ownMarksOf) const
    {
        std::span<const BlockID> order = cfg.reversePostOrder();
        if (programToReadComparisonsWith)
            return 0;
        auto hasMark = [&](ValueID id) { return at(id) || ownMarksOf(id, function.at(id)); };
        return static_cast<uint32_t>(std::ranges::find_if(order, [&](BlockID id) {
            const Block& block = function.at(id);
            return std::ranges::any_of(block.parameters, hasMark) || std::ranges::any_of(block.values, hasMark);
        }) - order.begin());
    }

    void run(NOESCAPE const Invocable<uint8_t(ValueID, const Value&)> auto& ownMarksOf)
    {
        std::span<const BlockID> order = cfg.reversePostOrder();
        uint32_t numberOfBlocks = static_cast<uint32_t>(order.size());
        for (uint32_t first = findFirstBlockWithMark(ownMarksOf); first < numberOfBlocks;) {
            // Only an edge that leads back brings a mark to a block that the walk has left.
            uint32_t firstBlockToWalkAgain = numberOfBlocks;
            for (uint32_t position = first; position < numberOfBlocks; ++position) {
                for (ValueID id : function.at(order[position]).values) {
                    const Value& value = function.at(id);
                    uint8_t found = ownMarksOf(id, value);
                    if (marksOfWhatIsMade)
                        found |= marksOfOperands(value) & marksOfWhatIsMade;
                    if (marksOfSameValue && casts.contains(value.opcode))
                        found |= at(value.operand(0)) & marksOfSameValue;
                    if (found)
                        add(id, found);
                }
                forEachOutgoingEdge(function, order[position], [&](Edge edge, const Target& target) {
                    hasChanged = false;
                    handOnMarks(edge, target);
                    if (hasChanged && cfg.reversePostOrderIndex(target.block) <= position)
                        firstBlockToWalkAgain = std::min(firstBlockToWalkAgain, cfg.reversePostOrderIndex(target.block));
                });
            }
            first = firstBlockToWalkAgain;
        }
    }
};

} // anonymous namespace

static BitVector walkFrom(const Function& function, const CFG& cfg, std::span<const ValueID> values, uint8_t marksOfWhatIsMade, uint8_t marksOfSameValue)
{
    BitVector isMarked(function.values.size());
    if (values.empty())
        return isMarked;
    Vector<uint8_t> marks(FillWith { }, function.values.size(), static_cast<uint8_t>(0));
    for (ValueID value : values)
        marks[value.index()] = 1;
    MarkWalk { function, cfg, marks, marksOfWhatIsMade, marksOfSameValue, 0, nullptr }.run([](ValueID, const Value&) -> uint8_t { return 0; });
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

static bool holdsCheck(const Function& function)
{
    return std::ranges::any_of(function.values, [](const Value& value) { return value.opcode == Opcode::Check; });
}

PredictedValues::PredictedValues(const Function& function)
    : m_numberOfValues(function.values.size())
    , m_hasChecks(holdsCheck(function))
{
    if (m_hasChecks)
        walk(function, CFG(function), { });
}

PredictedValues::PredictedValues(const Function& function, const CFG& cfg)
    : PredictedValues(function, cfg, Options { })
{
}

PredictedValues::PredictedValues(const Function& function, const CFG& cfg, Options options)
    : m_numberOfValues(function.values.size())
    , m_hasChecks(options.holdsCheck || holdsCheck(function))
{
    if (m_hasChecks)
        walk(function, cfg, options);
}

void PredictedValues::walk(const Function& function, const CFG& cfg, Options options)
{
    // Whoever reads comparisons asks whether their subject stands behind a check, an expectation or a test. Nobody else reads the last two.
    uint8_t marksOfSubjects = options.programToReadComparisonsWith ? BehindExpectation | BehindTest : 0;
    uint8_t marksOfWhatIsMade = BehindCheck | marksOfSubjects;
    m_marks = Vector<uint8_t>(FillWith { }, function.values.size(), static_cast<uint8_t>(0));
    MarkWalk { function, cfg, m_marks, marksOfWhatIsMade, 0, static_cast<uint8_t>(marksOfWhatIsMade & BehindTest), options.programToReadComparisonsWith }.run([&](ValueID id, const Value& value) -> uint8_t {
        if (value.opcode == Opcode::Check)
            return options.checksToLeaveOut && options.checksToLeaveOut->get(id.index()) ? 0 : BehindCheck;
        return value.opcode == Opcode::Expect ? marksOfWhatIsMade & BehindExpectation : 0;
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

void settleProofsBehindAnsweredChecks(Function& function, std::span<const ValueID> claimsBehindChecks, const PredictedValues* predicted)
{
    for (ValueID claim : claimsBehindChecks) {
        if (!predicted || !predicted->isBehindCheck(function.at(claim).valueOperand()))
            function.edit(claim, ValueEdit::Definition).setIsBehindChecks(false);
    }
}

void settleProofsBehindAnsweredChecks(Function& function)
{
    bool hasProofBehindChecks = std::ranges::any_of(function.values, [](const Value& value) {
        return value.opcode == Opcode::AssumeType && value.isBehindChecks();
    });
    if (hasProofBehindChecks)
        settleProofsBehindAnsweredChecks(function, CFG(function));
}

void settleProofsBehindOperandsThatWereOnlyKept(Function& function)
{
    settleProofsBehindAnsweredChecks(function);
}

bool isClaimOfResult(const Function& function, const TailCast& cast)
{
    if (cast.resolution != CastResolution::Assume || function.results.size() != 1)
        return false;
    std::optional<ProvenInEveryVersion> claimed = cast.declared.established();
    std::optional<ProvenInEveryVersion> result = function.result().established();
    return claimed && result && claimed->payload() == result->payload();
}

void becomeExpect(Value& value, ValueID operand, ExpectedType expected)
{
    value.become(Opcode::Expect);
    value.setValueOperand(operand);
    value.setExpectedType(expected);
}

static LayoutSetID withReceiversNobodyHasSeen(TypeArena& types, LayoutSetID stamp)
{
    const LayoutSet& receivers = types.at(stamp);
    if (!receivers.listsEveryLayout())
        return stamp;
    if (receivers.written().isEmpty())
        return { };
    return types.intern(LayoutSet::open(Vector<LayoutID>(receivers.written())));
}

static CalleeSetID withCalleesNobodyHasSeen(TypeArena& types, CalleeSetID stamp)
{
    std::optional<ProvenValue<Callees>> callees = types.at(stamp).all();
    if (!callees)
        return stamp;
    return types.intern(CalleeSet::open(Vector<FunctionID>(callees->value().functions), Vector<BuiltinID>(callees->value().builtins)));
}

ForgottenProofs forgetProofsBehind(Function& function, const CFG& cfg, TypeArena& types, std::span<const ValueID> values, const BitVector& checksThatGo, ExpectedTypes expectedTypes)
{
    if (values.empty())
        return { };
    return forgetProofsOf(function, types, valuesBehind(function, cfg, values), checksThatGo, expectedTypes);
}

void UndoLog::undo(Function& function)
{
    for (auto& [id, value] : m_values | std::views::reverse)
        function.edit(id) = WTF::move(value);
    for (auto& [id, terminator] : m_terminators | std::views::reverse)
        function.edit(id, BlockEdit::Terminator).terminator = WTF::move(terminator);
    m_values.clear();
    m_terminators.clear();
}

ForgottenProofs forgetProofsOf(Function& function, TypeArena& types, const BitVector& isBehind, const BitVector& checksThatGo, ExpectedTypes expectedTypes, UndoLog* undoLog)
{
    ForgottenProofs forgotten;
    ExpectedType nothing { types.withPrimitives(TypeTable::anything, Primitive::Absent) };
    auto edit = [&](ValueID id, OptionSet<ValueEdit> edits = anyValueEdit) -> Value& {
        if (undoLog)
            undoLog->willEdit(function, id);
        return function.edit(id, edits);
    };
    auto expect = [&](ValueID id, TypeID type) {
        Value& value = edit(id);
        if (expectedTypes == ExpectedTypes::Dropped)
            return becomeExpect(value, value.valueOperand(), nothing);
        ExpectedType expected { types.intern(types.at(type).withoutClassProofs()) };
        becomeExpect(value, value.valueOperand(), expected);
        value.setExpected(expected);
    };
    auto takesValueBehind = [&](const Instruction& user) {
        return std::ranges::any_of(user.operands(), [&](ValueID operand) { return operand && isBehind.get(operand.index()); });
    };
    // A validation goes with the claim that it tests.
    std::optional<HashSet<uint64_t>> claimsThatStay;
    auto testsClaimThatStays = [&](const Value& validation) {
        auto keyOf = [](ValueID operand, TypeID type) { return (static_cast<uint64_t>(operand.index()) << 32 | type.index()) + 1; };
        if (!claimsThatStay) {
            claimsThatStay.emplace();
            for (size_t index : isBehind) {
                const Value& claim = function.values[index];
                if (claim.opcode == Opcode::AssumeType && !claim.isBehindChecks())
                    claimsThatStay->add(keyOf(claim.valueOperand(), claim.provenType()));
            }
        }
        return claimsThatStay->contains(keyOf(validation.valueOperand(), validation.validatedType()));
    };

    for (size_t index : isBehind) {
        ValueID id(index);
        const Value& value = function.at(id);
        if (value.opcode == Opcode::ValidateType || value.opcode == Opcode::ValidateCallee) {
            if (value.opcode == Opcode::ValidateType && testsClaimThatStays(value))
                continue;
            if (undoLog)
                undoLog->willEdit(function, id);
            becomeUndefined(function, id);
            forgotten.hasErasedValidation = true;
        } else if (value.opcode == Opcode::Check) {
            if (checksThatGo.get(index))
                expect(id, value.expectedType().predicted());
        } else if (value.opcode == Opcode::AssumeType) {
            if (!value.isBehindChecks())
                continue;
            expect(id, value.provenType());
        } else if (value.opcode == Opcode::Upcast) {
            if (expectedTypes == ExpectedTypes::Kept)
                expect(id, value.castType());
            else
                edit(id).setCastType(nothing.predicted());
        }
    }

    for (uint32_t index = 0; index < function.blocks.size(); ++index) {
        const Block& block = function.blocks[index];
        const Terminator& terminator = block.terminator;
        if (terminator.kind == TerminatorKind::ReturnCall && isBehind.get(terminator.callOperand().index()) && terminator.casts().size()) {
            Vector<TailCast, 1> claims;
            for (TailCast cast : terminator.casts()) {
                if (isClaimOfResult(function, cast))
                    claims.append(cast);
            }
            if (undoLog)
                undoLog->willEditTerminator(function, BlockID(index));
            function.edit(BlockID(index), BlockEdit::Terminator).terminator.setCasts(claims.span());
        }
        for (ValueID id : block.values) {
            const Value& value = function.at(id);
            bool hasReceivers = Value::opcodesWithReceivers.contains(value.opcode) && value.receivers();
            bool hasCallees = Value::opcodesWithCallees.contains(value.opcode) && value.callees();
            if ((!hasReceivers && !hasCallees) || !takesValueBehind(value))
                continue;
            // Where the type of the check is expected no longer, what flow has seen is what is left to expect.
            if (hasReceivers)
                edit(id, ValueEdit::Definition).setReceivers(expectedTypes == ExpectedTypes::Dropped ? withReceiversNobodyHasSeen(types, value.receivers()) : LayoutSetID());
            if (!hasCallees)
                continue;
            if (CalleeSetID callees = withCalleesNobodyHasSeen(types, value.callees()); callees != value.callees())
                edit(id, ValueEdit::Definition).setCallees(callees);
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
