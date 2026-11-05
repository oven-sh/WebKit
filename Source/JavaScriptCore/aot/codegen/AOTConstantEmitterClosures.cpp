/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTConstantEmitter.h"

#if ENABLE(AOT)

#include "AOTBarrierEmitter.h"
#include "AOTCellAllocationEmitter.h"
#include "AOTClosure.h"
#include "AOTClosureTestEmitter.h"
#include "AOTConvention.h"
#include "AOTFunctionKinds.h"
#include "AOTFunctionTables.h"
#include "AOTGuardEmitter.h"
#include "AOTIRProgram.h"
#include "AOTInstance.h"
#include "AOTLimits.h"
#include "AOTNumbering.h"
#include "AOTRepresentationRules.h"
#include "AOTRuntimeCallEmitter.h"
#include "AOTShapeTestEmitter.h"
#include "AOTSlimClosure.h"
#include "AOTStubCalls.h"
#include "AOTStubsClosures.h"
#include "AOTTypeArena.h"
#include "MarkedSpace.h"
#include <algorithm>

#if COMPILER(CLANG)
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)
#endif

namespace JSC { namespace AOT { namespace Typed {

static constexpr unsigned slimClosureAllocatorIndex(unsigned numberOfCaptures)
{
    return MarkedSpace::sizeClassToIndex(SlimClosure::allocationSize(numberOfCaptures)) - 1;
}

static_assert(!slimClosureAllocatorIndex(0) && slimClosureAllocatorIndex(1) == 1 && slimClosureAllocatorIndex(2) == 1);
static_assert(slimClosureAllocatorIndex(ClosureRequest::maxSlimClosureCaptures) < ClosureWords::numberOfSlimClosureAllocators);
static_assert(ClosureRequest::maxNumberOfCaptures <= ClosureRequest::maxSlimClosureCaptures);
static_assert(ClosureRequest::allocationShift == 32 && ClosureRequest::isSlimBit == 31 && ClosureRequest::executableShift >= ClosureWord::numberOfFunctionBits);
static_assert(defaultLimitValue(LimitID::ClosureAllocationSites, hostArchitecture()) == 1u << (ClosureRequest::countShift - ClosureRequest::allocationShift));

static void storeClosureCaptureWords(Output& out, CellPointer closure, std::span<const LValue> wordsBySlot, const Machine::IndexedAbstractHeap& captureWords)
{
    for (size_t slot = 0; slot < wordsBySlot.size(); ++slot)
        out.store64(wordsBySlot[slot], out.address(closure.value(), captureWords[slot]));
}

// In the place of a capture that is removed the record lists a word that says nothing.
static bool closureHoldsWordForListedCapture(const Program& program, const CallingConventions& conventions, FunctionID function, unsigned index)
{
    return closureCaptureRepresentationOf(program, conventions, function, index) != Representation::Void;
}

void ConstantEmitter::generateNewClosure(const ValueContext& value)
{
    defineCell(value, closureCreatedBy(value));
}

CellPointer ConstantEmitter::closureCreatedBy(const ValueContext& value)
{
    FunctionID function = value->function();
    std::span<const ValueID> captures = value->capturesOperands();
    RELEASE_ASSERT(captures.size() == m_program.at(function).captures.size() && captures.size() <= Closure::maxNumberOfCaptures);

    CodeUnit entry = conventions().closureEntryOf(function);
    if (!conventions().exists(entry))
        return failWithoutCell(value, InternalErrorKind::OpcodeWithoutCode);

    NewClosureRequest request { function, m_plan.functionKind(m_program, entry), { }, value->storage(), value->closureForm(), captures };
    request.numberOfWords = closureCaptureCountsOf(m_program, conventions(), function).captures;
    if (value->isLent() && (request.form != ClosureForm::Slim || request.storage != AllocationStorage::Frame))
        return failWithoutCell(value, InternalErrorKind::OpcodeWithoutCode);
    switch (request.form) {
    case ClosureForm::Slim:
        return newSlimClosure(value, request);
    case ClosureForm::Full:
        if (request.storage != AllocationStorage::Heap)
            return newFullClosure(value, request);
        request.allocation = emitter<CellAllocationEmitter>().closureAllocationOf(request.numberOfWords);
        return canUseClosureStub(request) ? newClosureByStub(value, request) : newFullClosure(value, request);
    case ClosureForm::Deferred:
        return failWithoutCell(value, InternalErrorKind::OpcodeWithoutCode);
    case ClosureForm::Singleton:
        return singletonClosure(value, function);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

CellPointer ConstantEmitter::singletonClosure(const OperationContext& context, FunctionID function)
{
    CodeUnit entry = conventions().closureEntryOf(function);
    if (!function || !conventions().exists(entry))
        return failWithoutCell(context, InternalErrorKind::OpcodeWithoutCode);
    if (!m_plan.hasClosureCell(function))
        return failWithoutCell(context, InternalErrorKind::PlacedPayloadMissing);
    auto askStub = [&](bool isOnRarePath) {
        // The run time keeps the closure in its cell, and Image numbers the cells that code mentions: where the stub alone reads the cell, nothing else would.
        m_mentions.add(NumberSpace::ClosureCell, function);
        // The operation that takes the closure declares no write. Behind a call with its effects, a later read would still be answered with the empty word of the closure table.
        CallOptions options { .numberedImmediate = NumberedImmediate { m_mentions.add(NumberSpace::ClosureFunction, function) }, .isOnRarePath = isOnRarePath, .convertsNoCells = !!m_plan.functionKind(m_program, entry).isSlim };
        return CellPointer(emitter<RuntimeCallEmitter>().callStubWithoutSite(closureOfTable(), std::span<const LValue>(), options).first);
    };
    static constexpr OpcodeSet closures { Opcode::NewClosure };
    m_out.noteFormOf(closures, Form::Open, StateOfKnowledge::Singleton);
    CellPointer created = emitter<ClosureTestEmitter>().closureIfCreated(function);
    return either<CellPointer>(isNotEmpty(boxCell(created)), [&] {
        return created;
    }, [&] {
        return askStub(true);
    });
}

CellPointer ConstantEmitter::makeClosureOfField(const OperationContext& context, std::span<const DeferredFunction> functions, CellPointer object, uint32_t offsetOfSlot, bool capturesObject, std::optional<uint32_t> offsetOfEnvironment, NOESCAPE const ScopedLambda<CellPointer()>& makeInRunTime)
{
    // One stub makes the closures of one kind. Which function a closure is of is a number in the request.
    Vector<const DeferredFunction*, 4> made;
    std::optional<unsigned> slimClosureKind;
    ASSERT(!capturesObject || !offsetOfEnvironment);
    bool hasCapture = capturesObject || offsetOfEnvironment;
    if (offsetOfEnvironment && std::max(offsetOfSlot, *offsetOfEnvironment) > std::numeric_limits<uint16_t>::max())
        return makeInRunTime();
    RepresentationContext representations { m_program, m_plan, m_generator.inputs().types, m_unit };
    for (const DeferredFunction& deferred : functions) {
        CodeUnit entry = conventions().closureEntryOf(deferred.function);
        // A function with a cell in the closure table has one closure, which the run time keeps.
        if (!conventions().exists(entry) || m_program.at(deferred.function).captures.size() != hasCapture || m_plan.hasClosureCell(deferred.function))
            continue;
        FunctionKindData kind = m_plan.functionKind(m_program, entry);
        if (!kind.isSlim || kind.isConstructor || (slimClosureKind && *slimClosureKind != kind.slimClosureKind()))
            continue;
        if (hasCapture) {
            Representation representation = representationOfCapture(representations, deferred.function, 0);
            if (representation != Representation::Cell && representation != Representation::JSValue)
                continue;
        }
        slimClosureKind = kind.slimClosureKind();
        made.append(&deferred);
    }
    if (made.isEmpty())
        return makeInRunTime();

    ShapeTestEmitter& shapes = emitter<ShapeTestEmitter>();
    std::optional<Word32> shapeWord;
    auto isOf = [&](const DeferredFunction& deferred) {
        if (!shapeWord)
            shapeWord = shapes.shapeWordOf(object);
        Condition result;
        for (LayoutID layout : deferred.layouts)
            result = logicalOr(result, shapes.isShapeOf(*shapeWord, layout));
        return result;
    };
    auto closureNumberOf = [&](const DeferredFunction& deferred) {
        return numberOf(m_mentions.add(NumberSpace::ClosureFunction, deferred.function));
    };
    auto makeInStub = [&] {
        Word32 closureNumber = closureNumberOf(*made.last());
        for (size_t index = 0; index + 1 < made.size(); ++index)
            closureNumber = select(isOf(*made[index]), closureNumberOf(*made[index]), closureNumber);
        uint64_t offsets = static_cast<uint64_t>(offsetOfSlot) << 32 | static_cast<uint64_t>(offsetOfEnvironment.value_or(0)) << 48;
        Integer request = bitOr(zeroExtended(closureNumber), integer(static_cast<int64_t>(offsets)));
        StubKey stub = offsetOfEnvironment ? makeClosureOfFieldOverEnvironment(*slimClosureKind) : AOT::makeClosureOfField(*slimClosureKind, capturesObject);
        return CellPointer(callStub(context, stub, { request.value(), object.value() }, { .isOnRarePath = true, .accessesCollectorState = true }).first);
    };
    Condition isMadeInStub;
    if (made.size() != functions.size()) {
        for (const DeferredFunction* one : made)
            isMadeInStub = logicalOr(isMadeInStub, isOf(*one));
    }
    // The stub makes a closure of this instance. The closure of a field is of the instance of its object.
    if (!hasOneInstance()) {
        Condition belongs = emitter<GuardEmitter>().belongsToInstance(object);
        isMadeInStub = isMadeInStub ? logicalAnd(isMadeInStub, belongs) : belongs;
    }
    if (!isMadeInStub)
        return makeInStub();
    return either<CellPointer>(isMadeInStub, makeInStub, makeInRunTime);
}

bool ConstantEmitter::canUseClosureStub(const NewClosureRequest& request) const
{
    if (request.kind.closureStructure || request.numberOfWords > ClosureRequest::maxNumberOfCaptures)
        return false;
    for (unsigned index = 0; index < request.captures.size(); ++index) {
        if (closureHoldsWordForListedCapture(m_program, m_plan.conventions(), request.function, index) && machineTypeOf(representationOf(request.captures[index])) == Machine::Double)
            return false;
    }
    return true;
}

CellPointer ConstantEmitter::newSlimClosure(const ValueContext& value, const NewClosureRequest& request)
{
    unsigned count = request.numberOfWords;
    if (count > ClosureRequest::maxSlimClosureCaptures)
        return failWithoutCell(value, InternalErrorKind::OpcodeWithoutCode);

    uint64_t requestWord = static_cast<uint64_t>(request.kind.slimClosureKind()) << ClosureRequest::executableShift | 1ULL << ClosureRequest::isSlimBit;
    if (request.storage != AllocationStorage::Frame) {
        m_out.noteForm(Form::Closed, StateOfKnowledge::SlimOnHeap);
        return callClosureStub(value, request, requestWord);
    }

    m_out.noteForm(Form::Open, StateOfKnowledge::SlimInFrame);
    build().statistics().count(StatisticID::ClosuresInFrames);
    CellPointer cell = emitter<CellAllocationEmitter>().allocateInFrame(value, SlimClosure::allocationSize(count));
    CodeGenerator::InitializationScope scope(generator());
    emitter<CellAllocationEmitter>().storeHeader(cell, loadImmutable64(AOT_INSTANCE_OFFSET(slimClosureHeaders) + request.kind.slimClosureKind() * sizeof(uint64_t)));
    storeCaptures(cell, request, m_heaps.AOTSlimClosure_captures);
    storeField(cell, m_heaps.AOTSlimClosure_word, closureWordOf(request));
    return cell;
}

CellPointer ConstantEmitter::newClosureByStub(const ValueContext& value, const NewClosureRequest& request)
{
    m_out.noteForm(Form::Closed, StateOfKnowledge::FullOnHeap);
    unsigned executable = static_cast<unsigned>(FunctionTables::executableKind(request.kind));
    return callClosureStub(value, request, ClosureRequest::encode(0, executable, 0));
}

// Image counts the closure number and the row of the allocation, so `requestWord` has zero in the places of both.
CellPointer ConstantEmitter::callClosureStub(const ValueContext& value, const NewClosureRequest& request, uint64_t requestWord)
{
    RELEASE_ASSERT(!ClosureRequest::closureNumberOf(requestWord) && !(requestWord >> ClosureRequest::allocationShift));
    switch (request.storage) {
    case AllocationStorage::Heap:
        break;
    case AllocationStorage::Frame:
        return failWithoutCell(value, InternalErrorKind::OpcodeWithoutCode);
    }

    // The stub that makes a slim closure takes the number alone. In any other request the fields beside the number are the addend of its mention, and the row is the upper half.
    unsigned count = request.numberOfWords;
    bool isSlim = ClosureRequest::isSlim(requestWord);
    Integer word = zeroExtended(numberOf(closureNumberMentionedBy(request, isSlim ? 0 : static_cast<int32_t>(requestWord))));
    if (!isSlim)
        word = bitOr(word, Integer(m_out.shl(zeroExtended(numberOf(request.allocation.mention)).value(), ClosureRequest::allocationShift)));

    Vector<LValue, 6> operands;
    operands.append(word.value());
    operands.appendVector(captureWordsBySlot(request));
    return CellPointer(callStub(value, isSlim ? AOT::newSlimClosure(request.kind.slimClosureKind(), count) : newClosure(count), operands.span()).first);
}

CellPointer ConstantEmitter::newFullClosure(const ValueContext& value, const NewClosureRequest& request)
{
    m_out.noteForm(Form::Open, request.storage == AllocationStorage::Frame ? StateOfKnowledge::FullInFrame : StateOfKnowledge::FullWithOwnStructure);
    CellAllocationEmitter& cellAllocation = emitter<CellAllocationEmitter>();
    unsigned structure = request.kind.closureStructure;
    unsigned executable = static_cast<unsigned>(FunctionTables::executableKind(request.kind));
    // A constant is made where it is asked for, and whoever asks first for its cell calls out, where the host may collect. So every capture is at hand before there is a cell to fill.
    Vector<LValue, 4> captureWords = captureWordsBySlot(request);
    bool isOnHeap = false;
    CellPointer cell;
    switch (request.storage) {
    case AllocationStorage::Heap:
        cell = cellAllocation.allocate(value, request.allocation);
        isOnHeap = true;
        break;
    case AllocationStorage::Frame:
        cell = cellAllocation.allocateInFrame(value, Closure::allocationSize(request.numberOfWords));
        build().statistics().count(StatisticID::ClosuresInFrames);
        break;
    }

    {
        CodeGenerator::InitializationScope scope(generator());
        if (!isOnHeap || structure)
            cellAllocation.storeHeader(cell, loadImmutable64(AOT_INSTANCE_OFFSET(closures.closureHeaders) + structure * sizeof(uint64_t)));
        storeField(cell, m_heaps.JSNonArray_butterfly, integer(0));
        storeField(cell, m_heaps.JSCallee_scope, loadImmutablePointer(AOT_INSTANCE_OFFSET(globalObject)));
        storeField(cell, m_heaps.JSFunction_executableOrRareData, loadImmutablePointer(AOT_INSTANCE_OFFSET(closures.executables) + executable * sizeof(JSCell*)));
        storeClosureCaptureWords(m_out, cell, captureWords.span(), m_heaps.AOTClosure_captures);
        storeField(cell, m_heaps.AOTClosure_entry, closureWordOf(request));
    }
    if (isOnHeap)
        emitter<BarrierEmitter>().fenceAfterInitialization(value);
    return cell;
}

// In the form in which the closure keeps it, which is the form in which GetCapture reads it.
Operand ConstantEmitter::captureOf(const NewClosureRequest& request, unsigned index) const
{
    RepresentationContext context { m_program, m_plan, m_generator.inputs().types, m_unit };
    return m_generator.getIn(request.captures[index], representationOfCapture(context, request.function, index));
}

Vector<LValue, 4> ConstantEmitter::captureWordsBySlot(const NewClosureRequest& request)
{
    unsigned count = request.captures.size();
    Vector<LValue, 4> words(FillWith { }, request.numberOfWords, nullptr);
    for (unsigned i = 0; i < count; ++i) {
        if (!closureHoldsWordForListedCapture(m_program, conventions(), request.function, i))
            continue;
        Operand capture = captureOf(request, i);
        LType type = machineTypeOf(capture.representation);
        LValue word = type == Machine::Int32 ? m_out.zeroExtPtr(capture.value) : type == Machine::Double ? m_out.bitCast(capture.value, Machine::Int64) : capture.value;
        words[closureCaptureSlotOf(m_program, conventions(), request.function, i)] = word;
    }
    return words;
}

// It makes the captures while the cell is being filled: only for a cell in the frame, which the collector does not visit.
void ConstantEmitter::storeCaptures(CellPointer closure, const NewClosureRequest& request, const Machine::IndexedAbstractHeap& captureWords)
{
    RELEASE_ASSERT(request.storage == AllocationStorage::Frame);
    storeClosureCaptureWords(m_out, closure, captureWordsBySlot(request).span(), captureWords);
}

uint32_t ConstantEmitter::closureNumberMentionedBy(const NewClosureRequest& request, int32_t addend)
{
    return m_mentions.add(NumberSpace::ClosureFunction, request.function, addend);
}

Integer ConstantEmitter::closureWordOf(const NewClosureRequest& request)
{
    Pointer closureWords = loadImmutablePointer(AOT_INSTANCE_OFFSET(functionTables.closureWords));
    Integer closureNumber = zeroExtended(numberOf(closureNumberMentionedBy(request)));
    return Integer(m_out.load64(m_out.baseIndex(m_heaps.AOTImage_tables, closureWords.value(), closureNumber.value(), ScaleEight)));
}

} } } // namespace JSC::AOT::Typed

#if COMPILER(CLANG)
#pragma clang attribute pop
#endif

#endif // ENABLE(AOT)
