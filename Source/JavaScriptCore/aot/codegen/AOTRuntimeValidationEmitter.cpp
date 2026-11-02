/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRuntimeValidationEmitter.h"

#if ENABLE(AOT)

#include "AOTAccessEmitter.h"
#include "AOTBarrierEmitter.h"
#include "AOTClosureTestEmitter.h"
#include "AOTConversionEmitter.h"
#include "AOTEffectSummaries.h"
#include "AOTEffectsOfOperands.h"
#include "AOTFunctionAnalyses.h"
#include "AOTPlacement.h"
#include "AOTRepresentation.h"
#include "AOTRepresentationRules.h"
#include "AOTResultRules.h"
#include "AOTStubRegisters.h"
#include "AOTTypeTestEmitter.h"
#include "AOTTypeTestPlan.h"
#include "AOTValueIdentity.h"
#include "AOTValueSubstitution.h"
#include "AOTWrittenFunction.h"
#include "AllowMacroScratchRegisterUsage.h"
#include "B3CheckValue.h"
#include "B3StackmapGenerationParams.h"
#include "CCallHelpers.h"
#include <algorithm>
#include <utility>
#include <wtf/SetForScope.h>
#include <wtf/SharedTask.h>

namespace JSC { namespace AOT { namespace Typed {

RuntimeValidationEmitter::RuntimeValidationEmitter(CodeGenerator& generator)
    : OperationEmitter(generator)
    , m_framePointers(generator.function())
{
}

void RuntimeValidationEmitter::validateParameters(BlockID id, const Block& block)
{
    inheritCheckedClassProofs(id);
    for (ValueID parameter : block.parameters) {
        if (hasRegister(representationOf(parameter)))
            checkValue(parameter, get(parameter));
    }
}

static bool hasClassProofsToCheck(const Type& type)
{
    return std::ranges::any_of(type.classes, [](const ClassPart& part) {
        return part.contentsAreProven || !part.provenPristine.isEmpty();
    });
}

// A block with a single predecessor inherits the checks that still hold at the end of that predecessor.
void RuntimeValidationEmitter::inheritCheckedClassProofs(BlockID block)
{
    if (block == m_blockOfCheckedClassProofs)
        return;
    m_passedTypeTests.shrink(0);
    if (m_checkedClassProofsAtEnd.isEmpty())
        m_checkedClassProofsAtEnd.grow(analyses().cfg().numNodes());
    if (m_blockOfCheckedClassProofs)
        m_checkedClassProofsAtEnd[m_blockOfCheckedClassProofs.index()] = std::exchange(m_checkedClassProofs, { });
    m_blockOfCheckedClassProofs = block;
    std::span<const Edge> edges = analyses().cfg().incomingEdges(block);
    if (edges.size() == 1 && !edges[0].isHandlerEdge())
        m_checkedClassProofs = m_checkedClassProofsAtEnd[edges[0].from.index()];
}

static constexpr AbstractHeapSet heapsUnrelatedToClassProofs = heapsWhoseWritesNobodySees | compiledCodeHeaps | AbstractHeapSet { AbstractHeap::ValidationState };

template<typename User>
void RuntimeValidationEmitter::validateOperandsOf(const OperationContext& context, const User& user, ValueID ownedValue)
{
    inheritCheckedClassProofs(context.block());
    user.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        if (user.operandKind(operandIndex) == OperandKind::Guard) {
            checkGuardIsAvailable(context, operand, operandIndex);
            return;
        }
        if (operand != ownedValue && generator().isValidatedWhereLookedAt(operand) && user.operandUses(operandIndex).contains(OperandUse::Inspects))
            checkIsSpecified(operand);
        // The result of a cast that says so is tested where it is made, and a pointer that is not there fails that test. A number passes it whatever its bits.
        if constexpr (std::is_same_v<User, Value>) {
            if (user.opcode == Opcode::AssumeType && operandIndex == OperandIndex::AssumeType::value && typeOf(operand).mayBeUnspecified() && !m_program.at(user.provenType()).mayBeUnspecified() && hasRegister(representationOf(operand)))
                checkNumberIsSpecified(operand);
        }
        if (operand == ownedValue || generator().isValidatedWhereLookedAt(operand) || isDeferredClosure(m_function, operand) || emitter<AccessEmitter>().leavesKeyToRead(operand))
            return;
        // The bits of a value are those it was made with, and they were looked at there. What its type says about the heap may have changed since.
        const Type& type = typeOf(operand);
        if (type.layouts || !type.classes.isEmpty() || type.environment || generator().checksKeptRegisters())
            checkValue(operand, get(operand));
        // Nothing has written to the heap since an earlier operation made the same check.
        if (hasTestableBits(operand) && hasClassProofsToCheck(type) && !m_checkedClassProofs.contains(operand)) {
            checkClassProofs(operand, get(operand), type);
            m_checkedClassProofs.append(operand);
        }
        if (type.environment && representationOf(operand) == Representation::Cell)
            checkIsNotReleased(context, in<CellPointer>(operand), type.environment);
        bool mustNotBeFrameArray = refusesFrameArray(user, operand, operandIndex);
        if (!checkRetainedOperand(context, user, operand, operandIndex, mustNotBeFrameArray) && mustNotBeFrameArray)
            checkIsNotFrameCell(context, get(operand), std::nullopt, filterOfRefusedFrameArray(user));
    });
    Effects effects = context.effects().onAnyEdge();
    if (effects.mayRunOtherCode() || !effects.writes.containsOnly(heapsUnrelatedToClassProofs)) {
        m_checkedClassProofs.clear();
        m_passedTypeTests.shrink(0);
    }
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

void RuntimeValidationEmitter::forgetPassedTypeTestsBehind(const OperationContext& context)
{
    Effects effects = context.effects().onAnyEdge();
    if (effects.mayRunOtherCode() || !effects.writes.containsOnly(heapsUnrelatedToClassProofs))
        m_passedTypeTests.shrink(0);
}

void RuntimeValidationEmitter::runOutOfMemoryWhenCounted(const ValueContext& value)
{
    Effects effects = value.effects().onAnyEdge();
    if (!effects.mayThrow.isDeclaredOnlyForLackOfMemory() || effects.mayRunOtherCode() || value.opcode() == Opcode::LoopHint)
        return;
    Wire::SourceLocation location = m_function.locationOf(value.id());
    ModuleID module(location.module);
    LineColumn lineColumn = module ? m_program.lineColumnFor(module, location.position) : LineColumn { };
    auto count = [&](CountedLackOfMemory event) {
        callVoidOperation(value, RuntimeOperation::RunOutOfMemoryWhenCounted, { word32(static_cast<int32_t>(event)), word32(static_cast<int32_t>(location.module)), word32(static_cast<int32_t>(lineColumn.line)), word32(static_cast<int32_t>(lineColumn.column)) });
    };
    count(CountedLackOfMemory::Operation);
    // A text is flattened once, by whichever operation first needs its characters. A part of a flat text is left out: not every build makes it.
    value->forEachOperand([&](ValueID operand, uint32_t) {
        if (!typeOf(operand).isString() || typeOf(operand).isOpaque(OpaqueKind::FlatString))
            return;
        LBasicBlock isRope = newBlock();
        LBasicBlock isConcatenation = newBlock();
        LBasicBlock continuation = newBlock();
        Pointer fiber = loadField<Pointer>(in<CellPointer>(operand), m_heaps.JSString_value);
        m_out.branch(hasNoBits(fiber, JSString::isRopeInPointer).value(), normalEdge(continuation), unlikelyEdge(isRope));
        m_out.appendTo(isRope);
        m_out.branch(hasNoBits(fiber, JSRopeString::isSubstringInPointer).value(), normalEdge(isConcatenation), normalEdge(continuation));
        m_out.appendTo(isConcatenation);
        count(CountedLackOfMemory::Flattening);
        m_out.jump(continuation);
        m_out.appendTo(continuation);
    });
}

void RuntimeValidationEmitter::checkAssumedType(const ValueContext& value)
{
    ValueID source = value->valueOperand();
    const Type& type = m_program.at(value->provenType());
    if (type.isNever() || !hasTestableBits(source) || type.opaque || type.environment)
        return;
    m_blockOfAssumedType = value.block();
    checkHasType(value.id(), get(source), value->provenType(), ValidationFailureKind::AssumedType, source);
    m_blockOfAssumedType = { };
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
    Use use { context.valueID() ? Use::Kind::Operand : Use::Kind::TerminatorOperand, context.block(), context.valueID(), operandIndex, 0 };
    if (analyses().guardStateAt(guard, use) != GuardState::Unavailable || analyses().escape().gaveUp())
        return;
    if (context.valueID() && findReadsKeptForValidation(m_function).get(context.valueID().index()))
        return;
    // The passes removed a test based on the summaries of the time. Since then a call may have replaced operations in a callee's own body, and its summary may cover more than they did.
    // These analyses only judge, as the verifier's do.
    const EffectSummaries* effects = inputs().effectsByOperandTypes ? inputs().effectsByOperandTypes : analyses().context().effects;
    FunctionAnalyses finer(AnalysisContext { build(), m_program, &inputs().flow, effects, m_id, CallResults::MayAliasOperands, WorkLimits::OfBuild, SavedFrames::AreOutOfReach }, m_function);
    if (finer.guardStateAt(guard, use) != GuardState::Unavailable)
        return;
    fail(context, InternalErrorKind::GuardNotAvailable);
}

static bool callsOutlinedBody(const Program& program, const Value& user)
{
    return user.opcode == Opcode::CallDirect && program.at(user.function()).role == FunctionRole::OutlinedBody;
}

static bool mayBeArray(const Type& type)
{
    return type.anyObject || type.findClassPart(BuiltinClasses::Array);
}

// Returns whether the check refuses an array in a frame too.
bool RuntimeValidationEmitter::checkRetainedOperand(const OperationContext& context, const Value& user, ValueID operand, uint32_t operandIndex, bool refusesFrameArray)
{
    if (callsOutlinedBody(m_program, user))
        return false;
    // Such a function records whatever it was entered through. Of a function object in a frame the list keeps where it lay, and its readers follow nothing outside the cage of the heap: FunctionTables::closureOnHeapOf().
    if (user.opcode == Opcode::RecordCallee && recordsCalleeOnlyOnceRenamed(m_program, m_id))
        return false;
    FrameCellFilter asked = mayBeArray(typeOf(operand)) ? FrameCellFilter::AllButArrays : FrameCellFilter::All;
    auto checkStaysOutOfUnknownMemory = [&] {
        if (!mayLeakFramePointer(m_program, &inputs().flow, m_function, user, operandIndex))
            return false;
        checkIsNotFrameCell(context, get(operand), std::nullopt, refusesFrameArray ? FrameCellFilter::All : asked);
        return refusesFrameArray;
    };
    const OperandInfo& info = operandInfoOf(user.opcode, operandIndex);
    if (!info.uses.contains(OperandUse::Retains))
        return effectRuleOf(user.opcode) == EffectRule::Access && checkStaysOutOfUnknownMemory();
    switch (info.retainedIn) {
    case RetainedIn::UnknownMemory:
        return checkStaysOutOfUnknownMemory();
    case RetainedIn::Operand: {
        ValueID retainer = user.operand(*info.retainingOperand);
        RELEASE_ASSERT(retainer);
        checkIsNotFrameCell(context, get(operand), get(retainer), asked);
        return false;
    }
    case RetainedIn::Result:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool RuntimeValidationEmitter::checkRetainedOperand(const OperationContext& context, const Terminator& user, ValueID operand, uint32_t operandIndex, bool)
{
    // An outlined body is a part of the function that calls it, and may hand back what lies in the frame of that function.
    if (m_function.role == FunctionRole::OutlinedBody || !mayLeakFramePointer(m_program, &inputs().flow, m_function, user, operandIndex))
        return false;
    std::optional<uint32_t> numberOfResults = numberOfResultsOf(m_program, m_function, m_function.at(operand));
    if (!numberOfResults) {
        checkIsNotFrameCell(context, get(operand), std::nullopt);
        return true;
    }
    for (uint32_t index = 0; index < *numberOfResults; ++index)
        checkIsNotFrameCell(context, generator().resultAt(operand, index), std::nullopt);
    return false;
}

bool RuntimeValidationEmitter::refusesFrameArray(const Value& user, ValueID operand, uint32_t operandIndex)
{
    return mayBeArray(typeOf(operand)) && !mayBeGivenFrameArray(m_program, &inputs().flow, m_function, user, operandIndex);
}

// A branch or a switch looks at the value and hands it to nobody.
bool RuntimeValidationEmitter::refusesFrameArray(const Terminator& user, ValueID operand, uint32_t operandIndex)
{
    return m_function.role != FunctionRole::OutlinedBody && mayBeArray(typeOf(operand)) && !numberOfResultsOf(m_program, m_function, m_function.at(operand)) && user.kind != TerminatorKind::ReturnCall
        && mayLeakFramePointer(m_program, &inputs().flow, m_function, user, operandIndex);
}

void RuntimeValidationEmitter::validateResult(const ValueContext& value)
{
    Operand result = get(value.id());
    checkValue(value.id(), result);
    value->forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        const OperandInfo& info = operandInfoOf(value.opcode(), operandIndex);
        if (!info.uses.contains(OperandUse::Retains) || info.retainedIn != RetainedIn::Result)
            return;
        if (generator().isValidatedWhereLookedAt(operand) || isDeferredClosure(m_function, operand))
            return;
        checkIsNotFrameCell(value, get(operand), result);
    });
    emitter<BarrierEmitter>().validateAfterStore(value);
}

// The collector changes the state of a cell, and it and the compiler threads of the engine take the lock in the indexing byte, while the program runs.
static constexpr uint64_t bitsOfHeaderThatOnlyTheProgramChanges = ~(uint64_t { 0xff } << (JSCell::cellStateOffset() * 8)
    | static_cast<uint64_t>(IndexingTypeLockIsHeld | IndexingTypeLockHasParked) << (JSCell::indexingTypeAndMiscOffset() * 8));

// Something to read where the operand holds no object: the code of the image never changes.
Pointer RuntimeValidationEmitter::addressOfObjectOrOfCode(Operand operand, ComparedCells compared)
{
    Pointer code = loadImmutablePointer(AOT_INSTANCE_OFFSET(image.code));
    Integer bits(operand.value);
    Condition isCellOrEmpty = operand.is<CellPointer>() ? boolean(true) : isCell(operand.as<Boxed>());
    Pointer cell = select(logicalAnd(isCellOrEmpty, isNotZero(bits)), Pointer(operand.value), code);
    Word32 type(m_out.load8ZeroExt32(m_out.address(m_heaps.JSCell_header, cell.value(), JSCell::typeInfoTypeOffset())));
    static_assert(DerivedArrayType == ArrayType + 1);
    Condition isCompared = compared == ComparedCells::Objects ? aboveOrEqual(type, word32(ObjectType)) : belowOrEqual(sub(type, word32(ArrayType)), word32(DerivedArrayType - ArrayType));
    return select(isCompared, cell, code);
}

Integer RuntimeValidationEmitter::headerAt(Pointer address)
{
    return bitAnd(Integer(m_out.load64(m_out.address(m_heaps.JSCell_header, address.value(), 0))), static_cast<int64_t>(bitsOfHeaderThatOnlyTheProgramChanges));
}

Integer RuntimeValidationEmitter::wordBehindHeaderAt(Pointer address)
{
    static_assert(JSObject::butterflyOffset() == sizeof(uint64_t) && offsetOfSlimClosureWord == sizeof(uint64_t));
    return Integer(m_out.load64(m_out.address(m_heaps.JSObject_butterfly, address.value(), JSObject::butterflyOffset())));
}

// B3 keeps what it loaded from a cell's header, and from the word after it, across a call for which AOTHeaps::mayConvertCells() is false,
// and the word after an array's header across a call for which AOTHeaps::mayReplaceStorageOfArrays() is false.
RuntimeValidationEmitter::OperandCells RuntimeValidationEmitter::captureOperandCells(const OperationContext& context, ComparedCells compared)
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
        Pointer address = addressOfObjectOrOfCode(bits, compared);
        result.append({ context.valueID(), operand, address, compared == ComparedCells::Objects ? std::optional<Integer>(headerAt(address)) : std::nullopt, wordBehindHeaderAt(address) });
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
        Condition isAsBefore = equal(wordBehindHeaderAt(cell.address), cell.wordBehindHeader);
        if (cell.header)
            isAsBefore = logicalAnd(equal(headerAt(cell.address), *cell.header), isAsBefore);
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

RuntimeValidationEmitter::PhotographedOperands RuntimeValidationEmitter::photographOperands(const OperationContext& context, const Effects& effectsOfCall, ValueID alsoPerformed)
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

    EffectSummaries unknownSummaries;
    EffectContext effectContext { m_program, unknownSummaries };
    uint64_t heapsOfObjects = bitsOfHeapsIn(LocationMasks { perObjectHeaps });
    auto photographOperandsOf = [&](ValueID performed, ValueID madeByCall) {
        const Value& operation = m_function.at(performed);
        operation.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
            if (madeByCall && sameObjectThroughCasts(m_function, operand).valueInFrontOfFacts() == madeByCall)
                return;
            // As the passes ask it: an operation that names the operands it writes leaves the objects of the others alone.
            uint64_t writtenThroughOperand = bitsOfHeapsIn(memoryAccessedThrough(effectContext, m_function, operation, operandIndex, analyses().effectsOf(performed)).writes);
            photograph(operand, writtenByCall & (writtenThroughOperand | ~heapsOfObjects));
        });
    };
    photographOperandsOf(context.valueID(), ValueID());
    if (alsoPerformed)
        photographOperandsOf(alsoPerformed, context.valueID());
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
    Condition didNotStop = equal(countStopsForCollector(), stopsBeforeCall);
    failUnless(didNotStop, ValidationFailureKind::CallCollected, boxedConstant(jsNumber(CalledCode::encode(kind, callee))), context.valueID());
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
        reportUnreachable({ }, terminator->cutBy());
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

// The report prints only the class of what was found. It reads the class from a boxed value or a cell, and is told the representation of anything else.
static uint32_t encodeReportedFailure(ValidationFailureKind kind, Operand found)
{
    bool isRead = found.representation == Representation::JSValue || found.representation == Representation::Cell || found.representation == Representation::Cell32;
    return ReportedFailure::encode(kind, isRead ? Representation::Unchosen : found.representation);
}

void RuntimeValidationEmitter::failUnless(Condition condition, ValidationFailureKind kind, Operand found, ValueID id)
{
    if (condition.value()->isInt32(1))
        return;
    // Decoding takes blocks, and only the report needs what comes of it.
    if (found.representation == Representation::Cell32) {
        LBasicBlock doesNotHold = newBlock();
        LBasicBlock continuation = newBlock();
        m_out.branch(condition.value(), normalEdge(continuation), rareEdge(doesNotHold));
        m_out.appendTo(doesNotHold);
        reportFailure(kind, found, id);
        m_out.appendTo(continuation);
        return;
    }

    if (!m_reportGenerator) {
        m_reportGenerator = createSharedTask<B3::StackmapGeneratorFunction>([stubCalls = &m_records.stubCalls](CCallHelpers& jit, const B3::StackmapGenerationParams& params) {
            AllowMacroScratchRegisterUsage allowScratch(jit);
            for (unsigned index = 0; index < 4; ++index) {
                const B3::ValueRep& argument = params[index];
                GPRReg destination = operationArgumentGPR(index + 1);
                if (argument.isConstant()) {
                    jit.move(CCallHelpers::TrustedImm64(argument.value()), destination);
                    continue;
                }
                RELEASE_ASSERT(!index);
                if (argument.isGPR())
                    jit.move(argument.gpr(), destination);
                else
                    jit.load64(params.code().addressInFrame(argument.offsetFromFP()), destination);
            }
            stubCalls->reportChangedRegister(jit);
        });
    }
    LValue arguments[] { reportedValueOf(found).value(), m_out.constInt64(encodeReportedFailure(kind, found)), m_out.constInt64(m_id.index()), m_out.constInt64(id.index()) };
    B3::CheckValue* check = m_out.checkIsZero(logicalNot(condition).value());
    for (LValue argument : arguments)
        check->append(B3::ConstrainedValue(argument, B3::ValueRep::ColdAny));
    check->setGenerator(m_reportGenerator);
}

void RuntimeValidationEmitter::failUnlessHasType(Operand operand, const TypeTestPlan& plan, ValidationFailureKind kind, ValueID id, TypeID typeThatRunTimeTests)
{
    SetForScope passesChangedClosures(emitter<ClosureTestEmitter>().passesChangedClosures(), true);
    if (std::optional<Condition> condition = emitter<TypeTestEmitter>().tryEmitAsOneCondition(operand, plan, CodeShape::Fast)) {
        failUnless(*condition, kind, operand, id);
        return;
    }
    if (typeThatRunTimeTests && !generator().checksProofs()) {
        Integer hasType = callWithoutSite<Integer>(RuntimeOperation::ValueHasType, { reportedValueOf(operand), word32(static_cast<int32_t>(typeThatRunTimeTests.index())) });
        failUnless(isNotZero(hasType), kind, operand, id);
        return;
    }
    LBasicBlock doesNotHold = newBlock();
    LBasicBlock continuation = newBlock();
    OperationEmitter::branchOnType(operand, plan, { normalEdge(continuation), rareEdge(doesNotHold) });
    m_out.appendTo(doesNotHold);
    reportFailure(kind, operand, id);
    m_out.appendTo(continuation);
}

void RuntimeValidationEmitter::branchOnType(Operand operand, const TypeTestPlan& plan, BranchTargets targets)
{
    SetForScope passesChangedClosures(emitter<ClosureTestEmitter>().passesChangedClosures(), true);
    OperationEmitter::branchOnType(operand, plan, targets);
}

// The operation looks closer than generated code can, and returns where the statement holds.
void RuntimeValidationEmitter::askOperationUnless(Condition condition, std::span<const ValidationFailureKind> kinds, Operand found, ValueID id)
{
    LBasicBlock isUndecided = newBlock();
    LBasicBlock continuation = newBlock();
    m_out.branch(condition.value(), normalEdge(continuation), rareEdge(isUndecided));
    m_out.appendTo(isUndecided);
    for (ValidationFailureKind kind : kinds) {
        Operand arguments[] { reportedValueOf(found), reportedFailure(kind, found), word32(m_id.index()), word32(id.index()) };
        callWithoutSite(RuntimeOperation::ReportValidationFailure, arguments, Representation::Void, { .isOnRarePath = true, .reportMayReturn = true });
    }
    m_out.jump(continuation);
    m_out.appendTo(continuation);
}

void RuntimeValidationEmitter::checkPrecondition(const ValueContext& value, Condition holds)
{
    failUnless(holds, ValidationFailureKind::OperationPreconditionBroken, { }, value.id());
}

void RuntimeValidationEmitter::checkClaim(std::optional<CellPointer> about, Condition holds)
{
    failUnless(holds, ValidationFailureKind::OperationPreconditionBroken, about ? Operand(*about) : Operand(), generator().operationBeingGenerated());
}

void RuntimeValidationEmitter::checkBindingIsStored(const ValueContext& value, Condition isStored)
{
    failUnless(isStored, ValidationFailureKind::BindingReadBeforeStore, { }, value.id());
}

void RuntimeValidationEmitter::checkTypeTestsAgree(Condition agree, CellPointer tested)
{
    failUnless(agree, ValidationFailureKind::TypeTestsDiffer, tested, { });
}

void RuntimeValidationEmitter::reportFailure(ValidationFailureKind kind, Operand found, ValueID id)
{
    callReport(kind, found, id);
    m_out.unreachable();
}

void RuntimeValidationEmitter::callReport(ValidationFailureKind kind, Operand found, ValueID id)
{
    Operand arguments[] { reportedValueOf(found), reportedFailure(kind, found), word32(m_id.index()), word32(id.index()) };
    callWithoutSite(RuntimeOperation::ReportValidationFailure, arguments, Representation::Void, { .isOnRarePath = true });
}

// The caller ends the block.
void RuntimeValidationEmitter::reportUnreachable(ValueID id, PassID cutBy)
{
    callReport(ValidationFailureKind::UnreachableValue, cutBy == PassID::None ? Operand() : Operand(boxedConstant(jsNumber(static_cast<int32_t>(cutBy)))), id);
}

Word32 RuntimeValidationEmitter::reportedFailure(ValidationFailureKind kind, Operand found)
{
    return word32(static_cast<int32_t>(encodeReportedFailure(kind, found)));
}

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
    case Representation::DoubleWithPayload:
    case Representation::ImpureDouble:
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
