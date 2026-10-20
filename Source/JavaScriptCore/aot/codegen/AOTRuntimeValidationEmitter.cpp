/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRuntimeValidationEmitter.h"

#if ENABLE(AOT)

#include "AOTBarrierEmitter.h"
#include "AOTConversionEmitter.h"
#include "AOTFunctionAnalyses.h"
#include "AOTRepresentation.h"
#include "AOTRepresentationRules.h"
#include "AOTResultRules.h"
#include "AOTValueIdentity.h"

namespace JSC { namespace AOT { namespace Typed {

RuntimeValidationEmitter::RuntimeValidationEmitter(CodeGenerator& generator)
    : OperationEmitter(generator)
    , m_framePointers(generator.function())
{
}

void RuntimeValidationEmitter::validateParameters(BlockID, const Block& block)
{
    for (ValueID parameter : block.parameters) {
        if (hasRegister(representationOf(parameter)))
            checkValue(parameter, get(parameter));
    }
}

template<typename User>
void RuntimeValidationEmitter::validateOperandsOf(const OperationContext& context, const User& user, ValueID ownedValue)
{
    user.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        if (user.operandKind(operandIndex) == OperandKind::Guard) {
            checkGuardIsAvailable(context, operand, operandIndex);
            return;
        }
        if (operand == ownedValue || generator().mayBeUnspecified(operand) || isDeferredClosure(m_function, operand))
            return;
        checkValue(operand, get(operand));
        const Type& type = typeOf(operand);
        if (hasTestableBits(operand))
            checkClassProofs(operand, get(operand), type);
        if (type.environment && representationOf(operand) == Representation::Cell)
            checkIsNotReleased(context, in<CellPointer>(operand), type.environment);
        checkRetainedOperand(context, user, operand, operandIndex);
    });
}

void RuntimeValidationEmitter::validateOperands(const OperationContext& context)
{
    if (ValueID id = context.valueID()) {
        validateOperandsOf(context, m_function.at(id), { });
        checkReceiverOfSlotAccess(context, m_function.at(id));
        if (!m_framePointers.isEmpty() && isSuspensionPoint(id))
            poisonFrameCells({ context.block(), analyses().useLists().locationOf(id).index });
        return;
    }
    const Terminator& terminator = m_function.at(context.block()).terminator;
    validateOperandsOf(context, terminator, valueOwnedByTerminator(terminator));
}

void RuntimeValidationEmitter::checkGuardIsAvailable(const OperationContext& context, ValueID guard, uint32_t operandIndex)
{
    Use::Kind kind = context.valueID() ? Use::Kind::Operand : Use::Kind::TerminatorOperand;
    if (analyses().guardStateAt(guard, Use { kind, context.block(), context.valueID(), operandIndex, 0 }) == GuardState::Unavailable)
        fail(context, InternalErrorKind::GuardNotAvailable);
}

static bool callsOutlinedBody(const Program& program, const Value& user)
{
    return user.opcode == Opcode::CallDirect && program.at(user.function()).role == FunctionRole::OutlinedBody;
}

void RuntimeValidationEmitter::checkRetainedOperand(const OperationContext& context, const Value& user, ValueID operand, uint32_t operandIndex)
{
    if (callsOutlinedBody(m_program, user))
        return;
    const OperandInfo& info = operandInfoOf(user.opcode, operandIndex);
    if (!info.uses.contains(OperandUse::Retains)) {
        if (effectRuleOf(user.opcode) == EffectRule::Access && mayLeakFramePointer(m_program, &inputs().flow, m_function, user, operandIndex))
            checkIsNotFrameCell(context, get(operand), std::nullopt);
        return;
    }
    switch (info.retainedIn) {
    case RetainedIn::UnknownMemory:
        if (mayLeakFramePointer(m_program, &inputs().flow, m_function, user, operandIndex))
            checkIsNotFrameCell(context, get(operand), std::nullopt);
        return;
    case RetainedIn::Operand: {
        ValueID retainer = user.operand(*info.retainingOperand);
        RELEASE_ASSERT(retainer);
        checkIsNotFrameCell(context, get(operand), get(retainer));
        return;
    }
    case RetainedIn::Result:
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void RuntimeValidationEmitter::checkRetainedOperand(const OperationContext& context, const Terminator& user, ValueID operand, uint32_t operandIndex)
{
    if (!mayLeakFramePointer(m_program, &inputs().flow, m_function, user, operandIndex))
        return;
    std::optional<uint32_t> numberOfResults = numberOfResultsOf(m_program, m_function, m_function.at(operand));
    if (!numberOfResults) {
        checkIsNotFrameCell(context, get(operand), std::nullopt);
        return;
    }
    for (uint32_t index = 0; index < *numberOfResults; ++index)
        checkIsNotFrameCell(context, generator().resultAt(operand, index), std::nullopt);
}

void RuntimeValidationEmitter::validateResult(const ValueContext& value)
{
    Operand result = get(value.id());
    checkValue(value.id(), result);
    value->forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        const OperandInfo& info = operandInfoOf(value.opcode(), operandIndex);
        if (!info.uses.contains(OperandUse::Retains) || info.retainedIn != RetainedIn::Result)
            return;
        if (generator().mayBeUnspecified(operand) || isDeferredClosure(m_function, operand))
            return;
        checkIsNotFrameCell(value, get(operand), result);
    });
    emitter<BarrierEmitter>().validateAfterStore(value);
}

void RuntimeValidationEmitter::validateExit(const TerminatorContext& terminator)
{
    validateOperands(terminator);
    if (terminator.kind() == TerminatorKind::Unreachable)
        reportUnreachable({ });
    if (m_framePointers.isEmpty())
        return;

    switch (terminator.kind()) {
    case TerminatorKind::Return:
    case TerminatorKind::ReturnCall:
        poisonFrameCells(pointBeforeTerminatorOf(m_function, terminator.block()));
        return;
    case TerminatorKind::Throw:
    case TerminatorKind::Rethrow:
        if (!m_function.at(terminator.block()).handler)
            poisonFrameCells(pointBeforeTerminatorOf(m_function, terminator.block()));
        return;
    case TerminatorKind::Jump:
    case TerminatorKind::Branch:
    case TerminatorKind::Switch:
    case TerminatorKind::SwitchString:
    case TerminatorKind::Unreachable:
    case TerminatorKind::SwitchOnShape:
    case TerminatorKind::TryCallInline:
    case TerminatorKind::TestAndBranch:
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void RuntimeValidationEmitter::failUnless(Condition condition, ValidationFailureKind kind, Operand found, ValueID id)
{
    LBasicBlock doesNotHold = newBlock();
    LBasicBlock continuation = newBlock();
    m_out.branch(condition.value(), normalEdge(continuation), rareEdge(doesNotHold));
    m_out.appendTo(doesNotHold);
    reportFailure(kind, found, id);
    m_out.appendTo(continuation);
}

// The operation looks closer than generated code can, and returns where the statement holds.
void RuntimeValidationEmitter::askOperationUnless(Condition condition, ValidationFailureKind kind, Operand found, ValueID id)
{
    LBasicBlock isUndecided = newBlock();
    LBasicBlock continuation = newBlock();
    m_out.branch(condition.value(), normalEdge(continuation), rareEdge(isUndecided));
    m_out.appendTo(isUndecided);
    Operand arguments[] { reportedValueOf(found), word32(static_cast<int32_t>(kind)), word32(m_id.index()), word32(id.index()) };
    callWithoutSite(RuntimeOperation::ReportValidationFailure, arguments, Representation::Void, { .isOnRarePath = true, .reportMayReturn = true });
    m_out.jump(continuation);
    m_out.appendTo(continuation);
}

void RuntimeValidationEmitter::checkPrecondition(const ValueContext& value, Condition holds)
{
    failUnless(holds, ValidationFailureKind::OperationPreconditionBroken, { }, value.id());
}

void RuntimeValidationEmitter::checkBindingIsStored(const ValueContext& value, Condition isStored)
{
    failUnless(isStored, ValidationFailureKind::BindingReadBeforeStore, { }, value.id());
}

void RuntimeValidationEmitter::reportFailure(ValidationFailureKind kind, Operand found, ValueID id)
{
    callReport(kind, found, id);
    m_out.unreachable();
}

void RuntimeValidationEmitter::callReport(ValidationFailureKind kind, Operand found, ValueID id)
{
    Operand arguments[] { reportedValueOf(found), word32(static_cast<int32_t>(kind)), word32(m_id.index()), word32(id.index()) };
    callWithoutSite(RuntimeOperation::ReportValidationFailure, arguments, Representation::Void, { .isOnRarePath = true });
}

// The caller ends the block.
void RuntimeValidationEmitter::reportUnreachable(ValueID id)
{
    callReport(ValidationFailureKind::UnreachableValue, { }, id);
}

// The report prints only the class of what was found, which the IR already gives for everything but a cell.
Boxed RuntimeValidationEmitter::reportedValueOf(Operand found)
{
    switch (found.representation) {
    case Representation::JSValue:
        return found.as<Boxed>();
    case Representation::Cell:
        return boxCell(found.as<CellPointer>());
    case Representation::Cell32:
        return emitter<ConversionEmitter>().decode(found.as<NarrowReference>(), widestTypeOf(Representation::Cell32), Representation::JSValue).as<Boxed>();
    case Representation::Unchosen:
    case Representation::Void:
    case Representation::Boolean:
    case Representation::Int32:
    case Representation::Int64:
    case Representation::Double:
    case Representation::OptionalInt64:
    case Representation::OptionalDouble:
    case Representation::LiteralString:
    case Representation::RawWord:
        return boxedConstant(JSValue());
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
