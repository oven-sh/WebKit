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
