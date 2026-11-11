/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTOperandKinds.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDTypes.h"
#include "AOTCode.h"
#include "AOTHeldOperands.h"
#include "AOTIRProgram.h"
#include "AOTRepresentation.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct OperandKindFit {
    bool hasKind { false };
    bool mayBeUnspecified { false };
};

} // anonymous namespace

static uint16_t primitivesOfOperandKind(OperandKind kind)
{
    switch (kind) {
    case OperandKind::Any:
    case OperandKind::Declared:
    case OperandKind::EvalOperand:
    case OperandKind::Results:
    case OperandKind::Convertible:
        return Primitive::Defined;
    case OperandKind::Boolean:
        return Primitive::Boolean;
    case OperandKind::Integer:
        return Primitive::Integer;
    case OperandKind::Number:
        return Primitive::Number;
    case OperandKind::String:
        return Primitive::AnyString;
    case OperandKind::PropertyKey:
        return Primitive::AnyString | Primitive::AnySymbol;
    case OperandKind::Object:
    case OperandKind::Callable:
    case OperandKind::Array:
    case OperandKind::Layout:
    case OperandKind::Environment:
    case OperandKind::IteratorState:
    case OperandKind::Enumerator:
    case OperandKind::Exception:
    case OperandKind::ClassToken:
    case OperandKind::StringBuilder:
    case OperandKind::FlatString:
    case OperandKind::CodeAddress:
    case OperandKind::PropertyAddress:
    case OperandKind::Reciprocal:
    case OperandKind::BuilderCursor:
    case OperandKind::ViewVector:
    case OperandKind::ArrayStoragePointer:
    case OperandKind::MatchRecord:
    case OperandKind::Cursor:
    case OperandKind::KeyOrder:
        return 0;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

TypeID widestTypeOf(TypeArena& types, OperandKind kind)
{
    switch (kind) {
    case OperandKind::Boolean:
        return TypeTable::boolean;
    case OperandKind::Integer:
        return TypeTable::integer;
    case OperandKind::Number:
        return TypeTable::number;
    case OperandKind::String:
        return TypeTable::string;
    case OperandKind::PropertyKey:
        return TypeArena::fromPrimitives(Primitive::String | Primitive::Symbol);
    case OperandKind::Object:
        return TypeTable::object;
    case OperandKind::Callable:
        return TypeTable::function;
    case OperandKind::Array:
        return types.builtinClass(BuiltinClasses::Array);
    case OperandKind::Any:
    case OperandKind::Layout:
    case OperandKind::Declared:
    case OperandKind::Environment:
    case OperandKind::EvalOperand:
    case OperandKind::Results:
    case OperandKind::Convertible:
    case OperandKind::IteratorState:
    case OperandKind::Enumerator:
    case OperandKind::Exception:
    case OperandKind::ClassToken:
    case OperandKind::StringBuilder:
    case OperandKind::FlatString:
    case OperandKind::CodeAddress:
    case OperandKind::PropertyAddress:
    case OperandKind::Reciprocal:
    case OperandKind::BuilderCursor:
    case OperandKind::ViewVector:
    case OperandKind::ArrayStoragePointer:
    case OperandKind::MatchRecord:
    case OperandKind::Cursor:
    case OperandKind::KeyOrder:
        return { };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static constexpr bool operandKindAdmitsEnvironment(OperandKind kind)
{
    return kind == OperandKind::Layout || kind == OperandKind::Declared || kind == OperandKind::Environment || kind == OperandKind::EvalOperand || kind == OperandKind::Convertible;
}

static bool operandIsEnvironment(const Type& type)
{
    return type.environment && !type.primitives;
}

static bool operandHoldsOnlyObjects(const Type& type)
{
    return type.isJavaScriptValue() && !type.primitives;
}

static bool operandLayoutIsEnvironment(const Program* program, LayoutID layout)
{
    return layout && program && program->at(layout).isEnvironment();
}

static bool recordHasOperandKind(TypeArena& types, TypeID id, const Type& type, OperandKind kind, LayoutID layoutOfOperation)
{
    switch (kind) {
    case OperandKind::Any:
        return type.isJavaScriptValue();
    case OperandKind::Boolean:
    case OperandKind::Integer:
    case OperandKind::Number:
    case OperandKind::PropertyKey:
        return type.hasOnlyPrimitives(primitivesOfOperandKind(kind));
    case OperandKind::String:
        return type.hasOnlyPrimitives(primitivesOfOperandKind(kind)) || type.isOpaque(OpaqueKind::FlatString);
    case OperandKind::Object:
        return operandHoldsOnlyObjects(type);
    case OperandKind::Callable:
    case OperandKind::Array:
        return operandHoldsOnlyObjects(type) && types.isSubset(id, widestTypeOf(types, kind));
    case OperandKind::Layout:
        if (type.environment)
            return operandIsEnvironment(type) && type.environment == layoutOfOperation;
        return !operandLayoutIsEnvironment(types.program(), layoutOfOperation) && operandHoldsOnlyObjects(type);
    case OperandKind::Declared:
    case OperandKind::Results:
        return true;
    case OperandKind::Environment:
        return operandIsEnvironment(type);
    case OperandKind::EvalOperand:
        return type.isJavaScriptValue() || operandIsEnvironment(type) || type.isOpaque(OpaqueKind::ClassToken);
    case OperandKind::Convertible:
        return !type.opaque || holdsCells(representationOf(*type.opaque));
    case OperandKind::IteratorState:
    case OperandKind::Enumerator:
    case OperandKind::Exception:
    case OperandKind::ClassToken:
    case OperandKind::StringBuilder:
    case OperandKind::FlatString:
    case OperandKind::CodeAddress:
    case OperandKind::PropertyAddress:
    case OperandKind::Reciprocal:
    case OperandKind::BuilderCursor:
    case OperandKind::ViewVector:
    case OperandKind::ArrayStoragePointer:
    case OperandKind::MatchRecord:
    case OperandKind::Cursor:
    case OperandKind::KeyOrder:
        return type.isOpaque(*opaqueKindOf(kind));
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static OperandKindFit fitOfOperandKind(TypeArena& types, TypeID proven, OperandKind kind, LayoutID layoutOfOperation)
{
    if (holdsOnlyPrimitives(proven))
        return { hasOnlyPrimitivesIn(proven, primitivesOfOperandKind(kind) | Primitive::Unspecified), !!(primitivesInID(proven) & Primitive::Unspecified) };

    Type type = types.at(proven);
    if (type.mayBeUnspecified())
        return { fitOfOperandKind(types, types.withoutPrimitives(proven, Primitive::Unspecified), kind, layoutOfOperation).hasKind, true };
    return { recordHasOperandKind(types, proven, type, kind, layoutOfOperation), false };
}

bool hasKind(TypeArena& types, TypeID proven, OperandKind kind, LayoutID layoutOfOperation)
{
    if (!proven)
        return kind == OperandKind::Declared || kind == OperandKind::Results;
    return fitOfOperandKind(types, proven, kind, layoutOfOperation).hasKind;
}

static TypeID typeRequiredByOperandKind(TypeArena& types, OperandKind kind, LayoutID layoutOfOperation)
{
    if (TypeID widest = widestTypeOf(types, kind))
        return widest;
    if (kind == OperandKind::Any)
        return TypeTable::anything;
    if (kind == OperandKind::Layout)
        return operandLayoutIsEnvironment(types.program(), layoutOfOperation) ? types.forEnvironment(layoutOfOperation) : TypeTable::object;
    if (std::optional<OpaqueKind> opaque = opaqueKindOf(kind))
        return types.forOpaque(*opaque);
    return { };
}

static IllTypedOperand makeOperandKindMismatch(TypeArena& types, uint32_t operandIndex, TypeID proven, OperandKind kind, LayoutID layoutOfOperation)
{
    bool isMisplacedEnvironment = types.at(proven).environment && !operandKindAdmitsEnvironment(kind);
    InternalErrorKind error = isMisplacedEnvironment ? InternalErrorKind::InvalidEnvironmentUse : InternalErrorKind::OperandTypeMismatch;
    return { error, operandIndex, proven, typeRequiredByOperandKind(types, kind, layoutOfOperation) };
}

static bool operandIsEnvironmentThatMayBeAbsent(const TypeArena& types, TypeID proven)
{
    Type type = types.at(proven);
    return type.environment && type.primitives & Primitive::Absent;
}

static bool anyOperandTypeIsNever(OperandTypes proven)
{
    for (TypeID type : proven.types) {
        if (isNeverType(type))
            return true;
    }
    return false;
}

static bool operandPositionMayBeUnspecified(const TypingContext& context, TypeID known, const DeclaredType& declared)
{
    TypeID held = known ? known : context.atLevel(TypeLevel::Proven).typeIDOf(declared);
    return held && context.at(held).mayBeUnspecified();
}

static bool returnsOperandUnchanged(Opcode opcode, uint32_t operandIndex)
{
    if (operandUsesOf(opcode, operandIndex) != OperandUses { OperandUse::Returns })
        return false;
    return familyOf(opcode) == OperationFamily::Representation || opcode == Opcode::Results;
}

template<typename Operation>
bool handsOperandOn(const TypingContext& context, const Operation& operation, uint32_t operandIndex)
{
    Opcode opcode = opcodeOf(operation);
    if (opcode == Opcode::PutHeapFrameWord)
        return operandIndex == OperandIndex::PutHeapFrameWord::value;
    if ((opcode == Opcode::CallDirect || opcode == Opcode::AwaitCallBySavingFrame) && (returnsResultOfFunction(operation.role()) || operation.role() == CodeRole::CoroutineBody) && operandIndex >= numberOfNamedOperandsOf(opcode)) {
        uint32_t entryParameter = operandIndex - numberOfNamedOperandsOf(opcode);
        if (entryParameter < context.program.at(operation.function()).numberOfEntryParameters() && context.positions.entryParameterIsLeftOutOfCalls(operation.function(), entryParameter))
            return true;
    }
    if (opcode == Opcode::NewClosure && operandIndex >= numberOfNamedOperandsOf(opcode) && context.positions.entryParameterIsLeftOutOfCalls(operation.function(), operandIndex - numberOfNamedOperandsOf(opcode)))
        return true;
    if ((opcode == Opcode::GetEnvironmentField || opcode == Opcode::PutEnvironmentField) && operandIndex == OperandIndex::GetEnvironmentField::environment && context.positions.environmentLiesAmongVariables(operation.layout()))
        return true;
    if (opcode == Opcode::PutModuleVariable) {
        VariableID id = operation.variable();
        if (id.index() >= context.program.variables.size())
            return false;
        const Variable& variable = context.program.at(id);
        return variable.owner || operandPositionMayBeUnspecified(context, context.positions.variable(TypeLevel::Proven, id), variable.declared);
    }
    if (opcode == Opcode::PutEnvironmentField) {
        if (operandIndex != OperandIndex::PutEnvironmentField::value)
            return false;
        LayoutID layout = operation.layout();
        uint32_t field = operation.field();
        const Vector<Field>& fields = context.program.at(layout).fields;
        if (field >= fields.size())
            return false;
        return operandPositionMayBeUnspecified(context, context.positions.environmentField(TypeLevel::Proven, layout, field), fields[field].declared);
    }
    return isHeldOperand(operation, operandIndex) || returnsOperandUnchanged(opcode, operandIndex);
}

template<typename Operation>
static ALWAYS_INLINE bool everyOperandFitsAtAGlance(const TypingContext& context, const Operation& operation, OperandTypes proven)
{
    Opcode opcode = opcodeOf(operation);
    LayoutID environment;
    if (opcode == Opcode::GetEnvironmentField || opcode == Opcode::PutEnvironmentField) {
        environment = operation.layout();
        if (!operandLayoutIsEnvironment(&context.program, environment) || anyOperandTypeIsNever(proven))
            return false;
    }

    TypeArena& types = context.types;
    bool takesEveryOperandAsAny = testsItsOwnGuess(operation);
    uint32_t numberOfOperands = operation.numberOfOperands();
    const OperandInfosOfEntry infos = operandInfosByOpcode[static_cast<unsigned>(opcode)];
    for (uint32_t operandIndex = 0; operandIndex < numberOfOperands; ++operandIndex) {
        if (!operation.operand(operandIndex))
            continue;
        TypeID id = proven[operandIndex];
        if (!id)
            return false;

        OperandKind kind = takesEveryOperandAsAny ? OperandKind::Any : operandInfoIn(infos, operandIndex).kind;
        if (holdsOnlyPrimitives(id)) {
            if (primitivesInID(id) & ~(primitivesOfOperandKind(kind) & ~Primitive::Unspecified))
                return false;
        } else {
            const Type& type = types.recordOf(id);
            if (type.mayBeUnspecified())
                return false;
            switch (kind) {
            case OperandKind::Any:
                if (!type.isJavaScriptValue())
                    return false;
                break;
            case OperandKind::Object:
                if (!operandHoldsOnlyObjects(type))
                    return false;
                break;
            case OperandKind::Convertible:
                if (type.opaque)
                    return false;
                break;
            case OperandKind::Layout:
                if (!environment || !operandIsEnvironment(type) || type.environment != environment)
                    return false;
                break;
            case OperandKind::Callable:
            case OperandKind::Array:
                if (anyOperandTypeIsNever(proven) || !operandHoldsOnlyObjects(type) || !types.isSubset(id, widestTypeOf(types, kind)))
                    return false;
                break;
            case OperandKind::Boolean:
            case OperandKind::Integer:
            case OperandKind::Number:
            case OperandKind::PropertyKey:
            case OperandKind::String:
                if (!type.hasOnlyPrimitives(primitivesOfOperandKind(kind)))
                    return false;
                break;
            case OperandKind::Declared:
            case OperandKind::Results:
                break;
            default:
                return false;
            }
        }
        if (kind != OperandKind::Declared)
            continue;
        TypeID required = requiredTypeOfDeclaredOperand(context, operation, operandIndex);
        if (required && !types.isSubset(id, required))
            return false;
    }
    return true;
}

template<typename Operation>
std::optional<IllTypedOperand> findIllTypedOperand(const TypingContext& context, const Operation& operation, OperandTypes proven)
{
    static_assert(OperandIndex::GetEnvironmentField::environment == OperandIndex::PutEnvironmentField::environment);
    ASSERT(context.level == TypeLevel::Proven);

    if (everyOperandFitsAtAGlance(context, operation, proven))
        return std::nullopt;
    if (anyOperandTypeIsNever(proven))
        return std::nullopt;

    TypeArena& types = context.types;
    Opcode opcode = opcodeOf(operation);
    bool takesEveryOperandAsAny = testsItsOwnGuess(operation);
    bool storesIntoVariable = opcode == Opcode::PutModuleVariable || opcode == Opcode::PutEnvironmentField;
    LayoutID layout = PayloadWord::layout(opcode) ? operation.layout() : LayoutID();

    if ((opcode == Opcode::GetEnvironmentField || opcode == Opcode::PutEnvironmentField) && !operandLayoutIsEnvironment(&context.program, layout)) {
        uint32_t environment = OperandIndex::GetEnvironmentField::environment;
        return IllTypedOperand { InternalErrorKind::InvalidEnvironmentUse, environment, proven[environment], { } };
    }

    uint32_t numberOfOperands = operation.numberOfOperands();
    for (uint32_t operandIndex = 0; operandIndex < numberOfOperands; ++operandIndex) {
        if (!operation.operand(operandIndex))
            continue;

        TypeID type = proven[operandIndex];
        OperandKind kind = takesEveryOperandAsAny ? OperandKind::Any : operandKindOf(opcode, operandIndex);
        if (!type) {
            if (kind != OperandKind::Results)
                return IllTypedOperand { InternalErrorKind::OperandTypeMismatch, operandIndex, { }, typeRequiredByOperandKind(types, kind, layout) };
            if (opcode == Opcode::ResultAt)
                return IllTypedOperand { InternalErrorKind::ResultIndexOutOfRange, operandIndex, { }, { } };
            continue;
        }

        OperandKindFit fit = fitOfOperandKind(types, type, kind, layout);
        if (fit.mayBeUnspecified && !handsOperandOn(context, operation, operandIndex))
            return IllTypedOperand { InternalErrorKind::UnspecifiedValueInspected, operandIndex, type, { } };
        if (!fit.hasKind && !(opcode == Opcode::TypeTest && operandIsEnvironmentThatMayBeAbsent(types, type)))
            return makeOperandKindMismatch(types, operandIndex, type, kind, layout);
        if (kind != OperandKind::Declared)
            continue;

        TypeID required = requiredTypeOfDeclaredOperand(context, operation, operandIndex);
        if (!required)
            continue;
        if (fit.mayBeUnspecified && storesIntoVariable)
            required = types.withPrimitives(required, Primitive::Unspecified);
        if (!types.isSubset(type, required))
            return IllTypedOperand { InternalErrorKind::StoredTypeMismatch, operandIndex, type, required };
    }
    return std::nullopt;
}

static bool returnHandsOperandOn(const TypingContext& context, FunctionID returningFunction)
{
    if (!returningFunction)
        return false;
    const Function& function = context.program.at(returningFunction);
    return function.results.size() == 1 && operandPositionMayBeUnspecified(context, { }, function.result());
}

template<typename Terminator>
static bool terminatorHandsOperandsOn(const TypingContext& context, const Terminator& terminator, FunctionID returningFunction)
{
    TerminatorKind kind = terminatorKindOf(terminator);
    return kind == TerminatorKind::Return && returnHandsOperandOn(context, returningFunction);
}

template<typename Terminator>
std::optional<IllTypedOperand> findIllTypedOperandOfTerminator(const TypingContext& context, const Terminator& terminator, OperandTypes proven, FunctionID returningFunction)
{
    ASSERT(context.level == TypeLevel::Proven);

    if (anyOperandTypeIsNever(proven))
        return std::nullopt;

    TypeArena& types = context.types;
    TerminatorKind terminatorKind = terminatorKindOf(terminator);
    uint32_t numberOfOperands = terminator.numberOfOperands();
    for (uint32_t operandIndex = 0; operandIndex < numberOfOperands; ++operandIndex) {
        if (!terminator.operand(operandIndex))
            continue;

        TypeID type = proven[operandIndex];
        OperandKind kind = operandKindOf(terminatorKind, operandIndex);
        if (!type) {
            if (terminatorKind == TerminatorKind::Return)
                continue;
            return IllTypedOperand { InternalErrorKind::OperandTypeMismatch, operandIndex, { }, typeRequiredByOperandKind(types, kind, { }) };
        }

        OperandKindFit fit = fitOfOperandKind(types, type, kind, { });
        if (fit.mayBeUnspecified && !terminatorHandsOperandsOn(context, terminator, returningFunction))
            return IllTypedOperand { InternalErrorKind::UnspecifiedValueInspected, operandIndex, type, { } };
        if (!fit.hasKind)
            return makeOperandKindMismatch(types, operandIndex, type, kind, { });
    }
    return std::nullopt;
}

template bool handsOperandOn(const TypingContext&, const Draft&, uint32_t);
template bool handsOperandOn(const TypingContext&, const Operation&, uint32_t);
template std::optional<IllTypedOperand> findIllTypedOperand(const TypingContext&, const Draft&, OperandTypes);
template std::optional<IllTypedOperand> findIllTypedOperand(const TypingContext&, const Operation&, OperandTypes);
template std::optional<IllTypedOperand> findIllTypedOperandOfTerminator(const TypingContext&, const TerminatorDraft&, OperandTypes, FunctionID);
template std::optional<IllTypedOperand> findIllTypedOperandOfTerminator(const TypingContext&, const Terminator&, OperandTypes, FunctionID);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
