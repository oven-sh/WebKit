/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTVerifierInternal.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDTypes.h"
#include "AOTGuardAvailability.h"
#include "AOTRepresentation.h"
#include "AOTRepresentationRules.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint16_t propertyKeyPrimitives = Primitive::AnyString | Primitive::AnySymbol;

static bool isEnvironment(const Type& type)
{
    return type.environment && !type.primitives;
}

static bool hasOnlyObjects(const Type& type)
{
    return type.isJavaScriptValue() && !type.primitives;
}

static constexpr bool canBeEnvironment(OperandKind kind)
{
    return kind == OperandKind::Layout || kind == OperandKind::Declared || kind == OperandKind::Environment || kind == OperandKind::EvalOperand || kind == OperandKind::Convertible;
}

static bool hasCodeAfter(const Block& block, ValueID id)
{
    return block.values.last() != id || block.terminator.kind != TerminatorKind::Unreachable;
}

static bool canUseMultipleResults(const Function& function, const Use& use)
{
    switch (use.kind) {
    case Use::Kind::Operand:
        return function.at(use.user).operandKind(use.operandIndex) == OperandKind::Results;
    case Use::Kind::TerminatorOperand:
        return function.at(use.block).terminator.kind == TerminatorKind::Return;
    case Use::Kind::EdgeArgument:
    case Use::Kind::HandlerArgument:
    case Use::Kind::BlockSubject:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool isOnlyReturned(std::span<const Use> uses)
{
    return !uses.empty() && std::ranges::all_of(uses, [](const Use& use) {
        return use.kind == Use::Kind::TerminatorOperand;
    });
}

static bool hasValidTypeIDs(const FunctionVerifier& verifier, const Value& value)
{
    TypeID written = value.written();
    TypeID prediction = value.prediction();
    return verifier.isValid(value.type) && (!written || verifier.isValid(written)) && (!prediction || verifier.isValid(prediction));
}

static ValueID findValueWithInvalidTypeID(const FunctionVerifier& verifier, const Block& block)
{
    const Function& function = verifier.function();
    ValueID found;
    auto visit = [&](ValueID id) {
        if (!found && !hasValidTypeIDs(verifier, function.at(id)))
            found = id;
    };

    for (ValueID parameter : block.parameters)
        visit(parameter);
    for (ValueID id : block.values) {
        visit(id);
        function.at(id).forEachChild(visit);
    }
    block.forEachChildOutsideValues(visit);
    return found;
}

static std::optional<uint32_t> expectedNumberOfVariadicOperands(const Program& program, const Value& user)
{
    switch (user.opcode) {
    case Opcode::NewClosure:
        return user.isMadeOnDemand() ? 0 : static_cast<uint32_t>(program.at(user.function()).captures.size());
    case Opcode::NewObject:
    case Opcode::CloneAs:
        return static_cast<uint32_t>(user.fields().size());
    case Opcode::CallDirect:
        return program.at(user.function()).numberOfEntryParameters();
    case Opcode::CallGroup:
    case Opcode::AwaitCallBySavingFrame: {
        unsigned numberOfEntryParameters = program.at(user.function()).numberOfEntryParameters();
        return numberOfEntryParameters ? numberOfEntryParameters - 1 : 0;
    }
    case Opcode::PutSlots:
        return static_cast<uint32_t>(user.placedKeys().size());
    case Opcode::TryPutByIds:
    case Opcode::DefineAccessors:
        return static_cast<uint32_t>(user.keys().size());
    default:
        return std::nullopt;
    }
}

template<typename User>
static ValueID findOperandInWrongRepresentation(const Program& program, const Function& function, const User& user)
{
    ValueID found;
    user.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        if (found)
            return;
        Representation wanted = wantedRepresentationOf(program, function, user, operandIndex);
        if (hasRegister(wanted) && function.at(operand).representation != wanted)
            found = operand;
    });
    return found;
}

void FunctionVerifier::verifyTypes()
{
    std::span<const BlockID> reversePostOrder = analyses().cfg().reversePostOrder();
    for (BlockID blockID : reversePostOrder) {
        if (ValueID id = findValueWithInvalidTypeID(*this, m_function.at(blockID))) {
            fail(InternalErrorKind::IdOutOfRange, blockID, id, m_id.index());
            return;
        }
    }

    verifyEntryParameters();
    if (hasFailed())
        return;

    for (BlockID blockID : reversePostOrder) {
        const Block& block = m_function.at(blockID);
        verifyParameterTypes(blockID, block);
        for (ValueID parameter : block.parameters) {
            if (!hasFailed())
                verifyRepresentations(blockID, parameter, m_function.at(parameter));
        }
        if (hasFailed())
            return;

        for (ValueID id : block.values) {
            const Value& value = m_function.at(id);
            verifyOwnType(blockID, id, value);
            if (!hasFailed())
                verifyOperands(blockID, id, value);
            if (!hasFailed())
                verifyRequiredTypes(blockID, id, value);
            if (!hasFailed())
                verifyRepresentations(blockID, id, value);
            if (hasFailed())
                return;
        }

        verifyOperands(blockID, block.terminator);
        if (!hasFailed() && block.terminator.kind == TerminatorKind::Return)
            verifyReturn(blockID, block.terminator);
        if (!hasFailed() && m_function.representationsAreChosen) {
            if (ValueID operand = findOperandInWrongRepresentation(m_program, m_function, block.terminator))
                fail(InternalErrorKind::WrongRepresentation, blockID, { }, operand.index());
        }
        if (!hasFailed())
            verifyCodeAfterNever(blockID, block);
        if (hasFailed())
            return;
    }
}

void FunctionVerifier::verifyOwnType(BlockID block, ValueID id, const Value& value)
{
    const Type& stored = typeOf(id);
    Type computed = computeResultType(typing(), value);
    computed.canonicalize();
    if (computed != stored && (!isOptimizing() || !computed.isSubsetOf(stored, m_program))) {
        fail(InternalErrorKind::TypeOutOfDate, block, id);
        return;
    }

    for (TypeID narrowerID : { value.written(), value.prediction() }) {
        if (!narrowerID)
            continue;
        const Type& narrower = m_program.at(narrowerID);
        if (narrower == stored || !narrower.isSubsetOf(stored, m_program)) {
            fail(InternalErrorKind::TypeOutOfDate, block, id);
            return;
        }
    }

    if (!numberOfResultsOf(m_program, m_function, value))
        return;
    std::span<const Use> uses = analyses().useLists().usesOf(id);
    for (const Use& use : uses) {
        if (!canUseMultipleResults(m_function, use)) {
            fail(InternalErrorKind::OperandTypeMismatch, use.block, use.user, id.index());
            return;
        }
    }
    if (m_form == IRForm::AsRead && !isOptimizing() && value.opcode == Opcode::Results && !isOnlyReturned(uses))
        fail(InternalErrorKind::OperandTypeMismatch, block, id);
}

void FunctionVerifier::verifyParameterTypes(BlockID block, const Block& contents)
{
    if (contents.role == BlockRole::IterationCleanup && !typeOf(contents.subject).isOpaque(OpaqueKind::IteratorState)) {
        fail(InternalErrorKind::InvalidBlockRole, block, contents.subject);
        return;
    }
    if (block == entryBlock)
        return;

    for (Edge edge : analyses().cfg().incomingEdges(block)) {
        for (uint32_t index = 0; index < contents.parameters.size(); ++index) {
            ValueID parameter = contents.parameters[index];
            IncomingValue incoming = incomingValueOf(m_function, edge, index);
            switch (incoming.kind) {
            case IncomingValue::Kind::Argument:
                if (!need(InternalErrorKind::EdgeArgumentMismatch, edge.from, { }, incoming.argument, typeOf(parameter)))
                    return;
                break;
            case IncomingValue::Kind::EdgeResult:
                if (!computeTypeOnEdge(typing(), incoming.result).isSubsetOf(typeOf(parameter), m_program)) {
                    fail(InternalErrorKind::EdgeArgumentMismatch, edge.from, { }, parameter.index());
                    return;
                }
                break;
            }
        }
    }
}

void FunctionVerifier::verifyOperands(BlockID block, ValueID id, const Value& value)
{
    if (std::optional<uint32_t> expectedNumber = expectedNumberOfVariadicOperands(m_program, value)) {
        uint32_t number = value.numberOfVariadicOperands();
        if (number != *expectedNumber) {
            fail(InternalErrorKind::ArgumentCountMismatch, block, id, number);
            return;
        }
    }

    if (value.opcode == Opcode::GetEnvironmentField || value.opcode == Opcode::PutEnvironmentField) {
        LayoutKind kind = m_program.at(value.layout()).kind;
        bool isAsRead = m_form == IRForm::AsRead && !isOptimizing();
        if (!isEnvironmentKind(kind) || (isAsRead && kind != LayoutKind::Environment)) {
            fail(InternalErrorKind::InvalidEnvironmentUse, block, id);
            return;
        }
    }

    LayoutID layout = Value::opcodesWithLayout.contains(value.opcode) ? value.layout() : LayoutID();
    value.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        if (!hasFailed())
            verifyOperandKind(block, id, operandIndex, operand, value.operandKind(operandIndex), layout);
    });
}

void FunctionVerifier::verifyOperands(BlockID block, const Terminator& terminator)
{
    terminator.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        if (!hasFailed())
            verifyOperandKind(block, { }, operandIndex, operand, terminator.operandKind(operandIndex), { });
    });
}

void FunctionVerifier::verifyOperandKind(BlockID block, ValueID user, uint32_t, ValueID operand, OperandKind kind, LayoutID layoutPayload)
{
    const Type& type = typeOf(operand);
    bool hasKind = false;
    switch (kind) {
    case OperandKind::Any:
        hasKind = type.isJavaScriptValue();
        break;
    case OperandKind::Boolean:
        hasKind = type.hasOnlyPrimitives(Primitive::Boolean);
        break;
    case OperandKind::Integer:
        hasKind = type.hasOnlyPrimitives(Primitive::Integer);
        break;
    case OperandKind::Number:
        hasKind = type.hasOnlyPrimitives(Primitive::Number);
        break;
    case OperandKind::String:
        hasKind = type.hasOnlyPrimitives(Primitive::AnyString) || type.isOpaque(OpaqueKind::FlatString);
        break;
    case OperandKind::PropertyKey:
        hasKind = type.hasOnlyPrimitives(propertyKeyPrimitives);
        break;
    case OperandKind::Object:
        hasKind = hasOnlyObjects(type);
        break;
    case OperandKind::Callable:
        hasKind = type.isSubsetOf(m_program.at(TypeTable::function), m_program);
        break;
    case OperandKind::Array:
        hasKind = type.isSubsetOf(Type::builtinClass(BuiltinClasses::Array), m_program);
        break;
    case OperandKind::Layout: {
        bool isEnvironmentLayout = layoutPayload && m_program.at(layoutPayload).isEnvironment();
        hasKind = isEnvironmentLayout ? isEnvironment(type) && type.environment == layoutPayload : hasOnlyObjects(type);
        break;
    }
    case OperandKind::Declared:
        return;
    case OperandKind::Environment:
        hasKind = isEnvironment(type);
        break;
    case OperandKind::EvalOperand:
        hasKind = type.isJavaScriptValue() || isEnvironment(type) || type.isOpaque(OpaqueKind::ClassToken);
        break;
    case OperandKind::Convertible:
        hasKind = type.isJavaScriptValue() || isEnvironment(type) || (type.opaque && holdsCells(representationOf(*type.opaque)));
        break;
    case OperandKind::Results: {
        std::optional<uint32_t> numberOfResults = numberOfResultsOf(m_program, m_function, m_function.at(operand));
        if (numberOfResults && user && m_function.at(user).opcode == Opcode::ResultAt) {
            uint32_t resultIndex = m_function.at(user).index();
            if (resultIndex >= *numberOfResults) {
                fail(InternalErrorKind::ResultIndexOutOfRange, block, user, resultIndex);
                return;
            }
        }
        hasKind = numberOfResults.has_value();
        break;
    }
    case OperandKind::Guard:
        hasKind = type.isOpaque(OpaqueKind::Guard) || !proofsOfAllocation(m_program, m_function, m_function.at(operand)).isEmpty();
        break;
    case OperandKind::IteratorState:
    case OperandKind::Enumerator:
    case OperandKind::Exception:
    case OperandKind::ClassToken:
    case OperandKind::StringBuilder:
    case OperandKind::Cursor:
    case OperandKind::KeyOrder:
    case OperandKind::FlatString:
    case OperandKind::CodeAddress:
    case OperandKind::BuilderCursor:
        hasKind = type.isOpaque(*opaqueKindOf(kind));
        break;
    }
    if (hasKind)
        return;

    bool isMisplacedEnvironment = type.environment && !canBeEnvironment(kind);
    fail(isMisplacedEnvironment ? InternalErrorKind::InvalidEnvironmentUse : InternalErrorKind::OperandTypeMismatch, block, user, operand.index());
}

void FunctionVerifier::verifyRequiredTypes(BlockID block, ValueID id, const Value& value)
{
    TypingContext context = typing();
    value.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        if (hasFailed() || value.operandKind(operandIndex) != OperandKind::Declared)
            return;
        if (TypeID required = requiredTypeOfOperand(context, value, operandIndex))
            need(InternalErrorKind::StoredTypeMismatch, block, id, operand, context.types.at(required));
    });
}

void FunctionVerifier::verifyEntryParameters()
{
    auto& parameters = m_function.at(entryBlock).parameters;
    if (parameters.size() != m_function.numberOfEntryParameters()) {
        fail(InternalErrorKind::EntryParametersMismatch, entryBlock);
        return;
    }

    TypingContext context = typing();
    for (unsigned index = 0; index < parameters.size(); ++index) {
        TypeID declared = declaredEntryTypeOf(context, m_function, index);
        TypeID type = m_function.at(parameters[index]).type;
        if (type != declared && context.types.at(type) != context.types.at(declared)) {
            fail(InternalErrorKind::EntryParametersMismatch, entryBlock, parameters[index], index);
            return;
        }
    }
}

void FunctionVerifier::verifyReturn(BlockID block, const Terminator& terminator)
{
    ValueID returned = terminator.valueOperand();
    std::optional<uint32_t> numberOfReturnedResults;
    if (returned)
        numberOfReturnedResults = numberOfResultsOf(m_program, m_function, m_function.at(returned));

    bool returnsInnerValue = m_function.isUnsplitCoroutine();
    if (!returnsInnerValue && m_function.results.size() != 1) {
        if (numberOfReturnedResults != m_function.results.size()) {
            fail(InternalErrorKind::ReturnTypeMismatch, block);
            return;
        }
        for (uint32_t resultIndex = 0; resultIndex < *numberOfReturnedResults; ++resultIndex) {
            Type result = computeResultType(typing(), m_function.at(returned), resultIndex);
            if (!result.isSubsetOf(m_program.at(m_function.results[resultIndex].type), m_program)) {
                fail(InternalErrorKind::ReturnTypeMismatch, block);
                return;
            }
        }
        return;
    }

    if (numberOfReturnedResults) {
        fail(InternalErrorKind::ReturnTypeMismatch, block);
        return;
    }
    if (returnsInnerValue) {
        if (returned)
            verifyOperandKind(block, { }, 0, returned, OperandKind::Any, { });
        return;
    }

    const Type& declared = m_program.at(m_function.result().type);
    if (returned) {
        need(InternalErrorKind::ReturnTypeMismatch, block, { }, returned, declared);
        return;
    }
    if (!m_program.at(TypeTable::undefined).isSubsetOf(declared, m_program))
        fail(InternalErrorKind::ReturnTypeMismatch, block);
}

void FunctionVerifier::verifyRepresentations(BlockID block, ValueID id, const Value& value)
{
    if (!m_function.representationsAreChosen) {
        if (value.representation != Representation::Unchosen)
            fail(InternalErrorKind::WrongRepresentation, block, id);
        return;
    }

    if (!isLegalRepresentation(typeOf(id), value.representation)) {
        fail(InternalErrorKind::WrongRepresentation, block, id);
        return;
    }
    if (ValueID operand = findOperandInWrongRepresentation(m_program, m_function, value))
        fail(InternalErrorKind::WrongRepresentation, block, id, operand.index());
}

void FunctionVerifier::verifyCodeAfterNever(BlockID block, const Block& contents)
{
    for (ValueID id : contents.values) {
        const Value& value = m_function.at(id);
        if (value.type != TypeTable::never || resultRuleOf(value.opcode) == ResultRule::None)
            continue;
        if (hasCodeAfter(contents, id))
            fail(InternalErrorKind::ValueAfterNever, block, id);
        return;
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
