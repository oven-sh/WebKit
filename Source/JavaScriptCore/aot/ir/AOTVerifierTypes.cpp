/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTVerifierInternal.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDTypes.h"
#include "AOTFlowSiteData.h"
#include "AOTGuardAvailability.h"
#include "AOTNarrowFieldTypes.h"
#include "AOTRepresentation.h"
#include "AOTRepresentationRules.h"
#include "AOTThreads.h"
#include "AOTTypeClosure.h"
#include "AOTTypePropagation.h"
#include "AOTVector.h"
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

static bool canUseMultipleResults(const Function& function, const Use& use)
{
    if (use.kind == Use::Kind::Operand)
        return function.at(use.user).operandKind(use.operandIndex) == OperandKind::Results;
    return use.kind == Use::Kind::TerminatorOperand && function.at(use.block).terminator.kind == TerminatorKind::Return;
}

static bool isOnlyReturned(std::span<const Use> uses)
{
    return !uses.empty() && std::ranges::all_of(uses, [](const Use& use) {
        return use.kind == Use::Kind::TerminatorOperand;
    });
}

static bool hasExpectedTypeInsideType(const Program& program, const Value& value)
{
    TypeID expectedID = value.expected();
    if (!expectedID)
        return true;
    const Type& type = program.at(value.type);
    const Type& expected = program.at(expectedID);
    return expected != type && expected.isSubsetOf(type, program);
}

static ValueID findValueWithInvalidTypeID(const FunctionVerifier& verifier, const Block& block)
{
    const Function& function = verifier.function();
    ValueID found;
    auto visit = [&](ValueID id) {
        TypeID expected = function.at(id).expected();
        if (!found && (!verifier.isValid(function.at(id).type) || (expected && !verifier.isValid(expected))))
            found = id;
    };

    std::ranges::for_each(block.parameters, visit);
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
    case Opcode::CallTable:
        return program.at(user.function()).numberOfEntryParameters();
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

    bool mayHaveCodeAfterNever = waitsForGenericVersion(m_program, m_function);
    for (BlockID blockID : reversePostOrder) {
        const Block& block = m_function.at(blockID);
        verifyParameterTypes(blockID, block);
        for (ValueID parameter : block.parameters) {
            const Value& value = m_function.at(parameter);
            if (!hasFailed() && !hasExpectedTypeInsideType(m_program, value))
                fail(InternalErrorKind::TypeOutOfDate, blockID, parameter);
            if (!hasFailed())
                verifyRepresentations(blockID, parameter, value);
        }
        for (ValueID id : block.values) {
            const Value& value = m_function.at(id);
            if (!hasFailed())
                verifyOwnType(blockID, id, value);
            if (!hasFailed())
                verifyOperands(blockID, id, value);
            if (!hasFailed())
                verifyRequiredTypes(blockID, id, value);
            if (!hasFailed())
                verifyRepresentations(blockID, id, value);
        }
        if (hasFailed())
            return;

        verifyOperands(blockID, block.terminator);
        if (!hasFailed() && block.terminator.kind == TerminatorKind::Return && !mayHaveCodeAfterNever)
            verifyReturn(blockID, block.terminator);
        if (!hasFailed() && !mayHaveCodeAfterNever)
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
    bool isAsStored = computed == stored || computed == typing().types.adopt(stored);
    if ((!isAsStored && (!isOptimizing() || !computed.isSubsetOf(stored, m_program))) || !hasExpectedTypeInsideType(m_program, value)) {
        fail(InternalErrorKind::TypeOutOfDate, block, id);
        return;
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
    if (std::optional<uint32_t> expectedNumber = expectedNumberOfVariadicOperands(m_program, value); expectedNumber && value.numberOfVariadicOperands() != *expectedNumber) {
        fail(InternalErrorKind::ArgumentCountMismatch, block, id, value.numberOfVariadicOperands());
        return;
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
    auto hasKind = [&] {
        switch (kind) {
        case OperandKind::Any:
            return type.isJavaScriptValue();
        case OperandKind::Boolean:
            return type.hasOnlyPrimitives(Primitive::Boolean);
        case OperandKind::Integer:
            return type.hasOnlyPrimitives(Primitive::Integer);
        case OperandKind::Number:
            return type.hasOnlyPrimitives(Primitive::Number);
        case OperandKind::String:
            return type.hasOnlyPrimitives(Primitive::AnyString) || type.isOpaque(OpaqueKind::FlatString);
        case OperandKind::PropertyKey:
            return type.hasOnlyPrimitives(propertyKeyPrimitives);
        case OperandKind::Object:
            return hasOnlyObjects(type);
        case OperandKind::Callable:
            return type.isSubsetOf(m_program.at(TypeTable::function), m_program);
        case OperandKind::Array:
            return type.isSubsetOf(Type::builtinClass(BuiltinClasses::Array), m_program);
        case OperandKind::Layout:
            if (layoutPayload && m_program.at(layoutPayload).isEnvironment())
                return isEnvironment(type) && type.environment == layoutPayload;
            return hasOnlyObjects(type);
        case OperandKind::Declared:
            return true;
        case OperandKind::Environment:
            return isEnvironment(type);
        case OperandKind::EvalOperand:
            return type.isJavaScriptValue() || isEnvironment(type) || type.isOpaque(OpaqueKind::ClassToken);
        case OperandKind::Convertible:
            return type.isJavaScriptValue() || isEnvironment(type) || (type.opaque && holdsCells(representationOf(*type.opaque)));
        case OperandKind::Results:
            return numberOfResultsOf(m_program, m_function, m_function.at(operand)).has_value();
        case OperandKind::Guard:
            return type.isOpaque(OpaqueKind::Guard) || !proofsOfAllocation(m_program, m_function, m_function.at(operand)).isEmpty();
        case OperandKind::IteratorState:
        case OperandKind::Enumerator:
        case OperandKind::Exception:
        case OperandKind::ClassToken:
        case OperandKind::StringBuilder:
        case OperandKind::Cursor:
        case OperandKind::KeyOrder:
        case OperandKind::FlatString:
        case OperandKind::CodeAddress:
        case OperandKind::SourcePlan:
        case OperandKind::BuilderCursor:
            return type.isOpaque(*opaqueKindOf(kind));
        }
        RELEASE_ASSERT_NOT_REACHED();
    };
    if (kind == OperandKind::Results && user && m_function.at(user).opcode == Opcode::ResultAt) {
        std::optional<uint32_t> numberOfResults = numberOfResultsOf(m_program, m_function, m_function.at(operand));
        if (numberOfResults && m_function.at(user).index() >= *numberOfResults) {
            fail(InternalErrorKind::ResultIndexOutOfRange, block, user, m_function.at(user).index());
            return;
        }
    }
    if (hasKind())
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
        const Value& definition = m_function.at(operand);
        if (definition.opcode == Opcode::Check && context.types.at(definition.castType()).isNever())
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
    std::optional<uint32_t> numberOfReturnedResults = returned ? numberOfResultsOf(m_program, m_function, m_function.at(returned)) : std::nullopt;

    TypingContext context = typing();
    bool returnsInnerValue = m_function.isUnsplitCoroutine();
    if (!returnsInnerValue && m_function.results.size() != 1) {
        bool isInsideResults = numberOfReturnedResults == m_function.results.size();
        for (uint32_t resultIndex = 0; isInsideResults && resultIndex < *numberOfReturnedResults; ++resultIndex)
            isInsideResults = computeResultType(context, m_function.at(returned), resultIndex).isSubsetOf(context.typeOf(m_function.results[resultIndex]), m_program);
        if (!isInsideResults)
            fail(InternalErrorKind::ReturnTypeMismatch, block);
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

    const Type& declared = context.typeOf(m_function.result());
    if (returned)
        need(InternalErrorKind::ReturnTypeMismatch, block, { }, returned, declared);
    else if (!m_program.at(TypeTable::undefined).isSubsetOf(declared, m_program))
        fail(InternalErrorKind::ReturnTypeMismatch, block);
}

void FunctionVerifier::verifyRepresentations(BlockID block, ValueID id, const Value& value)
{
    bool isLegal = m_function.representationsAreChosen ? isLegalRepresentation(typeOf(id), value.representation) : value.representation == Representation::Unchosen;
    if (!isLegal)
        fail(InternalErrorKind::WrongRepresentation, block, id);
}

void FunctionVerifier::verifyCodeAfterNever(BlockID block, const Block& contents)
{
    for (ValueID id : contents.values) {
        const Value& value = m_function.at(id);
        if (value.type != TypeTable::never || resultRuleOf(value.opcode) == ResultRule::None)
            continue;
        if (contents.values.last() != id || contents.terminator.kind != TerminatorKind::Unreachable)
            fail(InternalErrorKind::ValueAfterNever, block, id);
        return;
    }
}

static bool fillsFieldsInsideTheirTypes(const Program& program, const DataNode::Object& object)
{
    if (!object.layout)
        return true;
    const Layout& layout = program.at(object.layout);
    return std::ranges::all_of(object.members, [&](const DataMember& member) {
        std::optional<uint32_t> field = layout.findField(member.key);
        if (!field)
            return true;
        const Field& filled = layout.fields[*field];
        return filled.declared.flags.contains(PositionFlags::IsExpected) || includesData(program, program.at(slotTypeOf(program, filled)), member.value);
    });
}

static LayoutID findLayoutFilledOutsideFieldTypes(const Program& program, const DataNode& root)
{
    LayoutID found;
    forEachDataNodeInPreorder(root, [&](uint32_t, const DataNode& node) {
        const auto* object = node.tryAs<DataNode::Object>();
        if (!found && object && !fillsFieldsInsideTheirTypes(program, *object))
            found = object->layout;
    });
    return found;
}

void ProgramVerifier::verifyDataTypes()
{
    Vector<InternalError> errors = collectInParallel<InternalError>(m_build.parallelLoops(), m_program.data.size(), [&](unsigned index, Vector<InternalError>& found) {
        if (LayoutID layout = findLayoutFilledOutsideFieldTypes(m_program, m_program.data[index].root))
            found.append(InternalError { .kind = InternalErrorKind::DataOutsideFieldType, .layout = layout, .count = index });
    });
    for (InternalError& error : errors)
        fail(WTF::move(error));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
