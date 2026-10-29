/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTVerifierInternal.h"

#if ENABLE(AOT)

#include "AOTAddGenericVersion.h"
#include "AOTFlowApply.h"
#include "AOTPredictedValues.h"
#include "AOTTypePropagation.h"
#include "AOTValueIdentity.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

static bool isOfKind(const Program& program, const Type& type, OperandKind kind)
{
    switch (kind) {
    case OperandKind::Boolean:
        return type.hasOnlyPrimitives(Primitive::Boolean);
    case OperandKind::Integer:
        return type.hasOnlyPrimitives(Primitive::Integer);
    case OperandKind::Number:
        return type.hasOnlyPrimitives(Primitive::Number);
    case OperandKind::String:
        return type.hasOnlyPrimitives(Primitive::AnyString) || type.isOpaque(OpaqueKind::FlatString);
    case OperandKind::PropertyKey:
        return type.hasOnlyPrimitives(Primitive::AnyString | Primitive::AnySymbol);
    case OperandKind::Object:
    case OperandKind::Layout:
        return !type.isJavaScriptValue() || !type.primitives;
    case OperandKind::Callable:
        return type.isSubsetOf(program.at(TypeTable::function), program);
    case OperandKind::Array:
        return type.isSubsetOf(Type::builtinClass(BuiltinClasses::Array), program);
    case OperandKind::Any:
    case OperandKind::Declared:
    case OperandKind::Environment:
    case OperandKind::EvalOperand:
    case OperandKind::Results:
    case OperandKind::Convertible:
    case OperandKind::IteratorState:
    case OperandKind::Enumerator:
    case OperandKind::Exception:
    case OperandKind::ClassToken:
    case OperandKind::Guard:
    case OperandKind::StringBuilder:
    case OperandKind::Cursor:
    case OperandKind::KeyOrder:
    case OperandKind::Fact:
    case OperandKind::FlatString:
    case OperandKind::CodeAddress:
    case OperandKind::JSONHolders:
    case OperandKind::BuilderCursor:
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// An operation that takes one kind of value has no form for any other, and the check of a type that the program declares may go at any time. So the kind of such an operand is owed to no such check:
// it holds in the body in which each of them lets everything through. A check without a place in the program is the compiler's own word about code that it has written itself.
void FunctionVerifier::verifyOperandKindsWithoutChecks()
{
    Vector<ValueID, 16> checks;
    for (BlockID block : analyses().cfg().reversePostOrder()) {
        for (ValueID id : m_function.at(block).values) {
            if (m_function.at(id).opcode == Opcode::Check && m_function.at(id).site())
                checks.append(id);
        }
    }
    if (checks.isEmpty())
        return;

    TypeArena& types = typing().types;
    Function withoutChecks(m_function);
    expectInsteadOfChecking(withoutChecks, analyses().cfg(), types, checks.span(), ExpectedTypes::Dropped);
    FunctionAnalyses analysesWithoutChecks(analyses().context(), withoutChecks);
    typeValuesWithoutCutting({ m_program, m_id, withoutChecks, analysesWithoutChecks, types, m_build });

    auto verify = [&](BlockID block, ValueID user, const auto& instruction) {
        instruction.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
            if (!hasFailed() && !isOfKind(m_program, types.at(withoutChecks.at(operand).type), instruction.operandKind(operandIndex)))
                fail(InternalErrorKind::OperandKindOwedToCheck, block, user, operand.index());
        });
    };
    for (BlockID block : analyses().cfg().reversePostOrder()) {
        for (ValueID id : withoutChecks.at(block).values)
            verify(block, id, withoutChecks.at(id));
        verify(block, { }, withoutChecks.at(block).terminator);
    }
}

// The check of a type may go at any time, so nothing that outlives it is owed to it. A position that every store is held to outlives the checks of whoever stores into it:
// what is stored there has the type of the position also in the body in which each check lets everything through. As read, the positions that take what a check turns away are not yet marked.
// A check that turns nothing away and stands behind none that does lets everything through as it is: what is proven behind it alone holds in that body too.
void FunctionVerifier::verifyStoredTypesWithoutChecks()
{
    Vector<ValueID, 16> checks;
    for (BlockID block : analyses().cfg().reversePostOrder()) {
        for (ValueID id : m_function.at(block).values) {
            if (m_function.at(id).opcode == Opcode::Check && mayTurnValueAway(m_program, m_function, m_function.at(id)))
                checks.append(id);
        }
    }
    if (checks.isEmpty())
        return;

    TypeArena& types = typing().types;
    Function withoutChecks(m_function);
    BitVector isBehindCheck = valuesBehind(m_function, analyses().cfg(), checks.span());
    forgetProofsOf(withoutChecks, types, isBehindCheck, isBehindCheck, ExpectedTypes::Dropped);
    FunctionAnalyses analysesWithoutChecks(analyses().context(), withoutChecks);
    typeValuesWithoutCutting({ m_program, m_id, withoutChecks, analysesWithoutChecks, types, m_build });
    TypingContext typingWithoutChecks { m_program, withoutChecks, types };
    auto verify = [&](BlockID block, ValueID user, ValueID stored, TypeID required) {
        if (!hasFailed() && stored && required && !types.at(withoutChecks.at(stored).type).isSubsetOf(types.at(required), m_program))
            fail(InternalErrorKind::StoredTypeOwedToCheck, block, user, stored.index());
    };
    for (BlockID block : analyses().cfg().reversePostOrder()) {
        for (ValueID id : withoutChecks.at(block).values) {
            const Value& value = withoutChecks.at(id);
            value.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
                if (value.operandKind(operandIndex) == OperandKind::Declared && takesOnlyWhatFlowHolds(m_program, value, operandIndex))
                    verify(block, id, operand, requiredTypeOfOperand(typingWithoutChecks, value, operandIndex));
            });
        }
        const Terminator& terminator = withoutChecks.at(block).terminator;
        if (terminator.kind == TerminatorKind::Return)
            verify(block, { }, terminator.valueOperand(), requiredTypeOfOperand(typingWithoutChecks, terminator, OperandIndex::Return::value));
    }
}

namespace {

// A pass has made the operation out of two, and it hands back what either of them would have. The mode beside the value says which.
struct ValueBesideMode {
    uint32_t value;
    uint32_t mode;
    // In this mode arrives also what nobody has sent in: what the awaited value rejects with.
    std::optional<int64_t> modeOfAnyValue;
};

} // anonymous namespace

static ValueBesideMode valueBesideModeOf(Opcode opcode)
{
    if (opcode == Opcode::AsyncGeneratorYield)
        return { static_cast<uint32_t>(YieldResult::Sent), static_cast<uint32_t>(YieldResult::Mode), static_cast<int64_t>(ResumeMode::Throw) };
    RELEASE_ASSERT(opcode == Opcode::StepAsyncGenerator);
    return { static_cast<uint32_t>(StepGeneratorResult::Value), static_cast<uint32_t>(StepGeneratorResult::Status), std::nullopt };
}

// What is known of the value without a test was proven of the result of one of the two operations. It holds where the mode has been asked, and has not answered that the value may be any.
void FunctionVerifier::verifyProofsOfValueBesideMode(BlockID blockOfSuspension, ValueID suspension)
{
    const UseLists& useLists = analyses().useLists();
    ValueBesideMode results = valueBesideModeOf(m_function.at(suspension).opcode);

    Vector<ValueID, 8> sameValues;
    Vector<ValueID, 4> modes;
    for (const Use& use : useLists.usesOf(suspension)) {
        if (use.kind != Use::Kind::Operand || m_function.at(use.user).opcode != Opcode::ResultAt)
            continue;
        if (m_function.at(use.user).index() == results.value)
            sameValues.append(use.user);
        else if (m_function.at(use.user).index() == results.mode)
            modes.append(use.user);
    }
    if (sameValues.isEmpty())
        return;

    // The mode goes on through casts, as an argument of an edge, and narrowed by a test of it.
    const CFG& cfg = analyses().cfg();
    size_t numberOfResults = modes.size();
    auto parameterTaking = [&](const Use& use) {
        return m_function.at(targetOf(m_function, use.edge()).block).parameters[use.operandIndex];
    };
    auto forEachBranchOn = [&](ValueID test, const auto& functor) {
        for (const Use& use : useLists.usesOf(test)) {
            if (use.kind == Use::Kind::TerminatorOperand && m_function.at(use.block).terminator.isConditionalBranch())
                functor(use.block);
        }
    };
    auto addModesNarrowedBehind = [&](BlockID block) {
        forEachEdgeResult(m_function, block, [&](const EdgeResult& result, ValueID parameter) {
            if (result.kind == EdgeResultKind::NarrowedOperand && modes.contains(result.subject))
                modes.appendIfNotContains(parameter);
        });
    };
    for (size_t next = 0; next < modes.size(); ++next) {
        for (const Use& use : useLists.usesOf(modes[next])) {
            if (use.isEdgeUse())
                modes.appendIfNotContains(parameterTaking(use));
            else if (use.kind == Use::Kind::TerminatorOperand)
                addModesNarrowedBehind(use.block);
            else if (use.kind == Use::Kind::Operand && isCastOrConversion(m_function.at(use.user).opcode) && !use.operandIndex)
                modes.appendIfNotContains(use.user);
            else if (use.kind == Use::Kind::Operand)
                forEachBranchOn(use.user, addModesNarrowedBehind);
        }
    }
    Vector<Edge, 4> arms;
    auto integerOf = [&](ValueID id) -> std::optional<int64_t> {
        while (isCastOrConversion(m_function.at(id).opcode))
            id = m_function.at(id).operand(0);
        return m_function.at(id).opcode == Opcode::Integer ? std::optional<int64_t>(m_function.at(id).integer()) : std::nullopt;
    };
    auto addArmsOfSwitch = [&](BlockID block, const Terminator& terminator) {
        auto cases = terminator.cases();
        bool otherwiseIsArm = !results.modeOfAnyValue || std::ranges::find(cases, *results.modeOfAnyValue) != cases.end();
        for (uint32_t index = 0; index < terminator.targets.size(); ++index) {
            if (index < cases.size() ? cases[index] != results.modeOfAnyValue : otherwiseIsArm)
                arms.append(Edge { block, index });
        }
    };
    auto addArmsOfComparison = [&](BlockID block, int64_t compared) {
        if (compared != results.modeOfAnyValue)
            arms.append(Edge { block, 0 });
        if (!results.modeOfAnyValue || compared == results.modeOfAnyValue)
            arms.append(Edge { block, 1 });
    };
    // The blocks that are reached from the operation by no arm.
    BitVector reachedByNoArm;
    auto findArms = [&] {
        arms.clear();
        for (ValueID mode : modes) {
            for (const Use& use : useLists.usesOf(mode)) {
                if (use.kind == Use::Kind::TerminatorOperand && m_function.at(use.block).terminator.kind == TerminatorKind::Switch)
                    addArmsOfSwitch(use.block, m_function.at(use.block).terminator);
                if (use.kind != Use::Kind::Operand || m_function.at(use.user).opcode != Opcode::StrictEq)
                    continue;
                if (std::optional<int64_t> compared = integerOf(m_function.at(use.user).operand(!use.operandIndex)))
                    forEachBranchOn(use.user, [&](BlockID block) { addArmsOfComparison(block, *compared); });
            }
        }
        reachedByNoArm = BitVector(m_function.blocks.size());
        reachedByNoArm.quickSet(blockOfSuspension.index());
        Vector<BlockID, 16> worklist { blockOfSuspension };
        while (!worklist.isEmpty()) {
            forEachOutgoingEdge(m_function, worklist.takeLast(), [&](Edge edge, const Target& target) {
                if (!arms.contains(edge) && !reachedByNoArm.quickSet(target.block.index()))
                    worklist.append(target.block);
            });
        }
    };
    Vector<ValueID, 8> copiesOfValue;
    BitVector isCopyOfValue;
    auto addCopyOfValue = [&](ValueID id) {
        if (!isCopyOfValue.set(id.index()))
            copiesOfValue.append(id);
    };
    auto findCopiesOfValue = [&] {
        std::ranges::for_each(sameValues, addCopyOfValue);
        for (size_t next = 0; next < copiesOfValue.size(); ++next) {
            for (const Use& use : useLists.usesOf(copiesOfValue[next])) {
                if (use.isEdgeUse())
                    addCopyOfValue(parameterTaking(use));
                else if (use.kind == Use::Kind::Operand && !use.operandIndex && isCastWithoutTest(m_function.at(use.user).opcode))
                    addCopyOfValue(use.user);
            }
        }
    };
    // An edge drops the value where it passes no copy of it and no copy is defined above the target: past the edge the value is named only after the operation has run again.
    auto dropsValue = [&](Edge edge) {
        if (copiesOfValue.isEmpty())
            findCopiesOfValue();
        const Target& target = targetOf(m_function, edge);
        if (target.arguments.containsIf([&](ValueID argument) { return isCopyOfValue.get(argument.index()); }))
            return false;
        return std::ranges::none_of(copiesOfValue, [&](ValueID value) {
            return analyses().dominators().strictlyDominates(useLists.locationOf(value).block, target.block);
        });
    };
    // A parameter stands for the mode where every edge that is reached by no arm hands the mode on or drops the value. On any other edge the mode has been asked, or the edge comes from elsewhere.
    auto holdsMode = [&](ValueID id) {
        ValueLocation location = useLists.locationOf(id);
        if (location.placement != ValuePlacement::Parameter)
            return modes.contains(m_function.at(id).operand(0));
        return std::ranges::all_of(cfg.incomingEdges(location.block), [&](Edge edge) {
            return !reachedByNoArm.quickGet(edge.from.index()) || modes.contains(valuePassedOn(m_function, edge, location.index)) || dropsValue(edge);
        });
    };
    for (;;) {
        findArms();
        auto lost = std::ranges::find_if(modes.begin() + numberOfResults, modes.end(), [&](ValueID mode) { return !holdsMode(mode); });
        if (lost == modes.end())
            break;
        modes.removeAt(lost - modes.begin());
    }
    auto isBehindArm = [&](BlockID block) {
        return !reachedByNoArm.quickGet(block.index());
    };

    for (size_t next = 0; next < sameValues.size(); ++next) {
        ValueID same = sameValues[next];
        for (const Use& use : useLists.usesOf(same)) {
            if (use.isEdgeUse()) {
                if (!arms.contains(use.edge()) && !isBehindArm(use.block))
                    sameValues.appendIfNotContains(parameterTaking(use));
                continue;
            }
            if (use.kind != Use::Kind::Operand || use.operandIndex)
                continue;
            const Value& user = m_function.at(use.user);
            bool validates = user.opcode == Opcode::ValidateType;
            if ((!validates && !isCastWithoutTest(user.opcode)) || isBehindArm(use.block))
                continue;
            if (!typeOf(same).isSubsetOf(validates ? m_program.at(user.validatedType()) : typeOf(use.user), m_program)) {
                fail(InternalErrorKind::ProofInFrontOfMode, use.block, use.user, suspension.index());
                return;
            }
            if (!validates)
                sameValues.append(use.user);
        }
    }
}

// A type that holds behind checks is written down only where a check stands in front of the value. Whoever takes the check away takes the type along.
void FunctionVerifier::verifyProofs()
{
    verifyOperandKindsWithoutChecks();
    if (hasFailed())
        return;

    if (m_form != IRForm::AsRead)
        verifyStoredTypesWithoutChecks();
    if (hasFailed())
        return;

    bool hasProofBehindChecks = std::ranges::any_of(m_function.values, [](const Value& value) {
        return value.opcode == Opcode::AssumeType && value.isBehindChecks();
    });
    if (!hasProofBehindChecks)
        return;

    PredictedValues predicted(m_function, analyses().cfg());
    for (BlockID block : analyses().cfg().reversePostOrder()) {
        for (ValueID id : m_function.at(block).values) {
            const Value& value = m_function.at(id);
            if (value.opcode == Opcode::AssumeType && value.isBehindChecks() && !predicted.isBehindCheck(value.valueOperand()))
                fail(InternalErrorKind::ProofWithoutCheck, block, id);
        }
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
