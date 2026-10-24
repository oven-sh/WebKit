/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRuntimeValidationEmitter.h"

#if ENABLE(AOT)

#include "AOTBarrierEmitter.h"
#include "AOTConversionEmitter.h"
#include "AOTEffectSummaries.h"
#include "AOTEffectsOfOperands.h"
#include "AOTFunctionAnalyses.h"
#include "AOTPlacement.h"
#include "AOTRepresentation.h"
#include "AOTRepresentationRules.h"
#include "AOTResultRules.h"
#include "AOTValueIdentity.h"
#include "AOTValueSubstitution.h"

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
        if (operand != ownedValue && generator().mayBeUnspecified(operand) && user.operandUses(operandIndex).contains(OperandUse::Inspects))
            checkIsSpecified(operand);
        if (operand == ownedValue || generator().mayBeUnspecified(operand) || isDeferredClosure(m_function, operand))
            return;
        // The bits of a value are those it was made with, and they were looked at there. What its type says about the heap may have changed since.
        const Type& type = typeOf(operand);
        if (type.layouts || !type.classes.isEmpty() || type.environment || generator().checksKeptRegisters())
            checkValue(operand, get(operand));
        if (hasTestableBits(operand))
            checkClassProofs(operand, get(operand), type);
        checkRetainedOperand(context, user, operand, operandIndex);
        checkIsNoFrameArray(context, user, operand, operandIndex);
    });
}

void RuntimeValidationEmitter::validateOperands(const OperationContext& context)
{
    if (ValueID id = context.valueID()) {
        // First: every patchpoint that stands at a suspension counts as the suspension for validateFrameAddresses(), and the checks of the operands have some.
        if (!m_framePointers.isEmpty() && isSuspensionPoint(id))
            poisonFrameCells({ context.block(), analyses().useLists().locationOf(id).index });
        validateOperandsOf(context, m_function.at(id), { });
        validateSlotAccess(context, m_function.at(id));
        checkShapeBehindGuard(context, m_function.at(id));
        return;
    }
    const Terminator& terminator = m_function.at(context.block()).terminator;
    validateOperandsOf(context, terminator, valueOwnedByTerminator(terminator));
}

void RuntimeValidationEmitter::checkSlotAccessWithoutGuard(const OperationContext& context)
{
    ValueID id = context.valueID();
    if (!id)
        return;
    const Value& access = m_function.at(id);
    if (familyOf(access.opcode) == OperationFamily::SlotAccess && !access.guardOperand())
        validateSlotAccess(context, access);
}

void RuntimeValidationEmitter::checkAssumedType(const ValueContext& value)
{
    ValueID source = value->valueOperand();
    const Type& type = m_program.at(value->castType());
    if (type.isNever() || !hasTestableBits(source) || type.opaque || type.environment || !type.functions.isEmpty())
        return;
    checkHasType(value.id(), get(source), type, ValidationFailureKind::AssumedType);
}

// The shape comes first: the slot of an object that is no receiver holds anything.
void RuntimeValidationEmitter::validateSlotAccess(const OperationContext& context, const Value& access)
{
    if (familyOf(access.opcode) != OperationFamily::SlotAccess)
        return;
    checkReceiverOfSlotAccess(context, access);
    ValueID object = access.objectOperand();
    if (hasTestableBits(object) && get(object).is<CellPointer>())
        checkRawNumberSlot(context, in<CellPointer>(object), m_placement.fieldAccess(access.fieldAccess()));
}

void RuntimeValidationEmitter::checkGuardIsAvailable(const OperationContext& context, ValueID guard, uint32_t operandIndex)
{
    Use::Kind kind = context.valueID() ? Use::Kind::Operand : Use::Kind::TerminatorOperand;
    if (analyses().guardStateAt(guard, Use { kind, context.block(), context.valueID(), operandIndex, 0 }) != GuardState::Unavailable)
        return;
    if (context.valueID() && findReadsKeptForValidation(m_function).get(context.valueID().index()))
        return;
    fail(context, InternalErrorKind::GuardNotAvailable);
}

static bool callsOutlinedBody(const Program& program, const Value& user)
{
    return user.opcode == Opcode::CallDirect && program.at(user.function()).role == FunctionRole::OutlinedBody;
}

// Of everything whose type an array fits, only arrays lie in frames in a validating build.
static bool mayBeArray(const Type& type)
{
    return type.anyObject || type.findClassPart(BuiltinClasses::Array);
}

void RuntimeValidationEmitter::checkRetainedOperand(const OperationContext& context, const Value& user, ValueID operand, uint32_t operandIndex)
{
    // checkIsNoFrameArray() asks more of these.
    if (callsOutlinedBody(m_program, user) || mayBeArray(typeOf(operand)))
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
    // An outlined body is a part of the function that calls it, and may hand back what lies in the frame of that function.
    if (m_function.role == FunctionRole::OutlinedBody || !mayLeakFramePointer(m_program, &inputs().flow, m_function, user, operandIndex))
        return;
    std::optional<uint32_t> numberOfResults = numberOfResultsOf(m_program, m_function, m_function.at(operand));
    if (!numberOfResults) {
        checkIsNotFrameCell(context, get(operand), std::nullopt);
        return;
    }
    for (uint32_t index = 0; index < *numberOfResults; ++index)
        checkIsNotFrameCell(context, generator().resultAt(operand, index), std::nullopt);
}

void RuntimeValidationEmitter::checkIsNoFrameArray(const OperationContext& context, const Value& user, ValueID operand, uint32_t operandIndex)
{
    if (mayBeArray(typeOf(operand)) && !mayBeGivenFrameArray(m_program, &inputs().flow, m_function, user, operandIndex))
        checkIsNotFrameCell(context, get(operand), std::nullopt);
}

void RuntimeValidationEmitter::checkIsNoFrameArray(const OperationContext& context, const Terminator& user, ValueID operand, uint32_t)
{
    if (m_function.role != FunctionRole::OutlinedBody && mayBeArray(typeOf(operand)) && !numberOfResultsOf(m_program, m_function, m_function.at(operand)) && user.kind != TerminatorKind::ReturnCall)
        checkIsNotFrameCell(context, get(operand), std::nullopt);
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
}

// The collector changes the state of a cell, and it and the compiler threads of the engine take the lock in the indexing byte, while the program runs.
static constexpr uint64_t bitsOfHeaderThatOnlyTheProgramChanges = ~(uint64_t { 0xff } << (JSCell::cellStateOffset() * 8)
    | static_cast<uint64_t>(IndexingTypeLockIsHeld | IndexingTypeLockHasParked) << (JSCell::indexingTypeAndMiscOffset() * 8));

// Something to read where the operand holds no object: the code of the image never changes.
Pointer RuntimeValidationEmitter::addressOfObjectOrOfCode(Operand operand)
{
    Pointer code = loadImmutablePointer(AOT_INSTANCE_OFFSET(image.code));
    Integer bits(operand.value);
    Condition isCellOrEmpty = operand.is<CellPointer>() ? boolean(true) : isCell(operand.as<Boxed>());
    Pointer cell = select(logicalAnd(isCellOrEmpty, isNotZero(bits)), Pointer(operand.value), code);
    Word32 type(m_out.load8ZeroExt32(m_out.address(m_heaps.JSCell_header, cell.value(), JSCell::typeInfoTypeOffset())));
    return select(aboveOrEqual(type, word32(ObjectType)), cell, code);
}

Integer RuntimeValidationEmitter::headerAt(Pointer address)
{
    return bitAnd(Integer(m_out.load64(m_out.address(m_heaps.JSCell_header, address.value(), 0))), integer(static_cast<int64_t>(bitsOfHeaderThatOnlyTheProgramChanges)));
}

Integer RuntimeValidationEmitter::wordBehindHeaderAt(Pointer address)
{
    static_assert(JSObject::butterflyOffset() == sizeof(uint64_t) && offsetOfSlimClosureWord == sizeof(uint64_t));
    return Integer(m_out.load64(m_out.address(m_heaps.JSObject_butterfly, address.value(), JSObject::butterflyOffset())));
}

// B3 keeps what it has loaded from the header of a cell, and from the word behind it, across a call of which AOTHeaps::mayConvertCells() says no.
RuntimeValidationEmitter::OperandCells RuntimeValidationEmitter::captureOperandCells(const OperationContext& context)
{
    OperandCells result;
    if (!context.valueID())
        return result;
    m_function.at(context.valueID()).forEachOperand([&](ValueID operand, uint32_t) {
        if (!hasTestableBits(operand) || !typeOf(operand).hasObjects())
            return;
        Operand bits = get(operand);
        if (!bits.is<CellPointer>() && !bits.is<Boxed>())
            return;
        Pointer address = addressOfObjectOrOfCode(bits);
        result.append({ context.valueID(), operand, address, headerAt(address), wordBehindHeaderAt(address) });
    });
    return result;
}

void RuntimeValidationEmitter::checkCellsWereNotConverted(const OperandCells& beforeCall)
{
    if (beforeCall.isEmpty())
        return;

    B3::PatchpointValue* forgetsLoads = m_out.patchpoint(B3::Void);
    forgetsLoads->effects = B3::Effects::none();
    B3HeapList converted;
    m_heaps.appendHeapsOfConvertedCells(converted);
    m_heaps.decoratePatchpointWrite(converted.span(), forgetsLoads);
    forgetsLoads->setRunsWithoutFrame(true);
    forgetsLoads->setGenerator([](CCallHelpers&, const B3::StackmapGenerationParams&) { });

    for (const OperandCell& cell : beforeCall) {
        Condition isAsBefore = logicalAnd(equal(headerAt(cell.address), cell.header), equal(wordBehindHeaderAt(cell.address), cell.wordBehindHeader));
        failUnless(isAsBefore, ValidationFailureKind::OperationConvertedCell, get(cell.operand), cell.operation);
    }
}

static uint64_t bitsOfHeapsIn(const LocationMasks& locations)
{
    uint64_t bits = 0;
    for (unsigned heap = 0; heap < numberOfEnumerators<AbstractHeap>; ++heap) {
        if (locations.contains(static_cast<AbstractHeap>(heap)))
            bits |= 1ull << heap;
    }
    return bits;
}

RuntimeValidationEmitter::PhotographedOperands RuntimeValidationEmitter::photographOperands(const OperationContext& context, const Effects& effectsOfCall)
{
    PhotographedOperands result;
    uint64_t writtenByCall = bitsOfHeapsIn(effectsOfCall.writes);
    auto photograph = [&](ValueID operand, uint64_t written) {
        if (result.size() == PhotographedCall::maxOperands || result.contains(operand) || !hasTestableBits(operand) || !typeOf(operand).hasObjects())
            return;
        Operand bits = get(operand);
        if (!bits.is<CellPointer>() && !bits.is<Boxed>())
            return;
        Operand arguments[] { reportedValueOf(bits), integer(static_cast<int64_t>(written)), word32(static_cast<int32_t>(result.size())) };
        callWithoutSite(RuntimeOperation::PhotographOperand, arguments, Representation::Void, { });
        result.append(operand);
    };

    if (!context.valueID()) {
        m_function.at(context.block()).terminator.forEachOperand([&](ValueID operand, uint32_t) {
            photograph(operand, writtenByCall);
        });
        return result;
    }

    const Value& operation = m_function.at(context.valueID());
    EffectSummaries unknownSummaries;
    EffectContext effectContext { m_program, unknownSummaries };
    uint64_t heapsOfObjects = bitsOfHeapsIn(LocationMasks { perObjectHeaps });
    operation.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        // As the passes ask it: an operation that names the operands it writes leaves the objects of the others alone.
        uint64_t writtenThroughOperand = bitsOfHeapsIn(memoryAccessedThrough(effectContext, m_function, operation, operandIndex, context.effects()).writes);
        photograph(operand, writtenByCall & (writtenThroughOperand | ~heapsOfObjects));
    });
    return result;
}

void RuntimeValidationEmitter::compareWithPhotographs(const OperationContext& context, const PhotographedOperands& photographed, uint32_t callee)
{
    for (unsigned position = 0; position < photographed.size(); ++position) {
        Operand arguments[] { reportedValueOf(get(photographed[position])), word32(static_cast<int32_t>(PhotographedCall::encode(position, callee))), word32(m_id.index()), word32(context.valueID().index()) };
        callWithoutSite(RuntimeOperation::CompareOperandWithPhotograph, arguments, Representation::Void, { });
    }
}

Integer RuntimeValidationEmitter::countStopsForCollector()
{
    return callWithoutSite<Integer>(RuntimeOperation::CountStopsForCollector, { });
}

void RuntimeValidationEmitter::checkCollectorDidNotStop(const OperationContext& context, Integer stopsBeforeCall, CalledCode::Kind kind, uint32_t callee)
{
    failUnless(equal(countStopsForCollector(), stopsBeforeCall), ValidationFailureKind::CallCollected, boxedConstant(jsNumber(CalledCode::encode(kind, callee))), context.valueID());
}

void RuntimeValidationEmitter::noteFailedGuard(Opcode opcode, uint32_t ordinal)
{
    Operand arguments[] { word32(m_id.index()), word32(static_cast<int32_t>(opcode)), word32(static_cast<int32_t>(ordinal)) };
    callWithoutSite(RuntimeOperation::NoteFailedGuard, arguments, Representation::Void, { .isOnRarePath = true });
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
