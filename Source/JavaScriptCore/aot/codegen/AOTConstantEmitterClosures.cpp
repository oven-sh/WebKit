/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTConstantEmitter.h"

#if ENABLE(AOT)

#include "AOTBarrierEmitter.h"
#include "AOTCellAllocationEmitter.h"
#include "AOTCellFormat.h"
#include "AOTClosure.h"
#include "AOTClosureTestEmitter.h"
#include "AOTClosuresLent.h"
#include "AOTConvention.h"
#include "AOTConversionEmitter.h"
#include "AOTEnvironmentEmitter.h"
#include "AOTFunctionKinds.h"
#include "AOTFunctionTables.h"
#include "AOTGuardEmitter.h"
#include "AOTHintEmitter.h"
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
static_assert(ClosureRequest::maxNumberOfCaptures <= ClosureRequest::maxSlimClosureCapturesInRegisters);
static_assert(ClosureRequest::allocationShift == 32 && ClosureRequest::isSlimBit == 31 && ClosureRequest::executableShift >= ClosureWord::numberOfFunctionBits);
static_assert(defaultLimitValue(LimitID::ClosureAllocationSites, hostArchitecture()) == 1u << (ClosureRequest::countShift - ClosureRequest::allocationShift));

static constexpr int64_t stateOfFrameCellInHeader = static_cast<int64_t>(static_cast<uint64_t>(frameCellState) << 8 * JSCell::cellStateOffset());
static_assert(frameCellState == UINT8_MAX && JSCell::cellStateOffset() == sizeof(uint64_t) - 1);

static void storeClosureCaptureWords(Output& out, CellPointer closure, std::span<const LValue> wordsBySlot, const Machine::IndexedAbstractHeap& captureWords, size_t firstSlot = 0)
{
    for (size_t slot = firstSlot; slot < wordsBySlot.size(); ++slot)
        out.store64(wordsBySlot[slot], out.address(closure.value(), captureWords[slot]));
}

static void storeThatClosureInFrameHasNoCopyInHeap(Output& out, const AOTHeaps& heaps, CellPointer closure, ptrdiff_t offsetOfCaptures, unsigned numberOfCaptures)
{
    out.store64(out.int64Zero, out.address(heaps.frameSlots, closure.value(), offsetOfCopyInHeapOfClosureInFrame(offsetOfCaptures, numberOfCaptures)));
}

static bool closureHoldsWordForListedCapture(const Program& program, const CallingConventions& conventions, FunctionID function, unsigned index)
{
    return closureCaptureRepresentationOf(program, conventions, function, index) != Representation::Void;
}

void ConstantEmitter::generateNewClosure(const ValueContext& value)
{
    CellPointer closure = closureCreatedBy(value);
    if (!generator().hasFailed() && emitter<HintEmitter>().shouldEmitLifetimeEnds() && lentClosureMayStandInHeap(m_code, value.valueID()) && value->loan() != LoanOfClosure::None)
        emitter<HintEmitter>().noteThatCellIsLent(value, closure);
    defineCell(value, closure);
}

bool ConstantEmitter::closureLentLiesInFrame(ValueID closure)
{
    const Operation& made = operationAt(closure);
    return made.opcode == Opcode::NewClosure && made.storage() == AllocationStorage::Frame && made.loan() != LoanOfClosure::None
        && !(emitter<HintEmitter>().shouldEmitLifetimeEnds() && lentClosureMayStandInHeap(m_code, closure));
}

CellPointer ConstantEmitter::closureCreatedBy(const ValueContext& value)
{
    FunctionID function = value->function();
    std::span<const ValueID> captures = value->capturesOperands();
    RELEASE_ASSERT((captures.empty() && value->closureForm() == ClosureForm::Singleton) || captures.size() == m_program.at(function).captures.size());
    RELEASE_ASSERT(captures.size() <= Closure::maxNumberOfCaptures);

    CodeUnit entry = conventions().closureEntryOf(function);
    if (!conventions().exists(entry))
        return failWithoutCell(value, InternalErrorKind::OpcodeWithoutCode);

    bool endsLifetimeAfterCall = emitter<HintEmitter>().shouldEmitLifetimeEnds() && lentClosureMayStandInHeap(m_code, value.valueID());
    if (endsLifetimeAfterCall)
        generator().scheduleClosureLifetimeEnd(value.valueID(), callThatBorrowsClosure(m_code, value.valueID()));
    NewClosureRequest request { function, m_plan.functionKind(m_program, entry), { }, endsLifetimeAfterCall ? AllocationStorage::Heap : value->storage(), value->closureForm(), captures };
    request.numberOfWords = closureCaptureCountsOf(m_program, conventions(), function).captures;
    if (request.form != ClosureForm::Deferred)
        emitter<EnvironmentEmitter>().markEnvironmentsThatClosureKeeps(value);
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
        m_mentions.add(NumberSpace::ClosureCell, function);
        CallOptions options { .numberedImmediate = NumberedImmediate { m_mentions.add(NumberSpace::ClosureFunction, function) }, .isOnRarePath = isOnRarePath, .convertsNoCells = !!m_plan.functionKind(m_program, entry).isSlim };
        return CellPointer(emitter<RuntimeCallEmitter>().callStubWithoutSite(closureOfTable(), std::span<const LValue>(), options).first);
    };
    static constexpr OpcodeSet closures { Opcode::NewClosure };
    m_out.noteFormOf(closures, Form::Open, StateOfKnowledge::Singleton);
    ClosureTestEmitter& tests = emitter<ClosureTestEmitter>();
    if (m_plan.usesWideReferences()) {
        CellPointer created = tests.closureIfCreated(function);
        return either<CellPointer>(isNotEmpty(boxCell(created)), [&] {
            return created;
        }, [&] {
            return askStub(true);
        });
    }
    Word32 tableEntry = tests.closureTableEntry(function);
    return either<CellPointer>(Condition(m_out.notZero32(tableEntry.value())), [&] {
        return tests.closureInTableEntry(tableEntry);
    }, [&] {
        return askStub(true);
    });
}

CellPointer ConstantEmitter::makeClosureOfField(const OperationContext& context, std::span<const DeferredFunction> functions, CellPointer object, uint32_t offsetOfSlot, bool capturesObject, std::optional<uint32_t> offsetOfEnvironment, NOESCAPE const ScopedLambda<CellPointer()>& makeInRunTime)
{
    Vector<const DeferredFunction*, 4> made;
    std::optional<unsigned> slimClosureKind;
    ASSERT(!capturesObject || !offsetOfEnvironment);
    bool hasCapture = capturesObject || offsetOfEnvironment;
    if (offsetOfEnvironment && std::max(offsetOfSlot, *offsetOfEnvironment) > std::numeric_limits<uint16_t>::max())
        return makeInRunTime();
    RepresentationContext representations { m_program, m_plan, m_generator.inputs().types, m_unit };
    for (const DeferredFunction& deferred : functions) {
        CodeUnit entry = conventions().closureEntryOf(deferred.function);
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
    if (request.kind.closureStructureOfKind() || request.numberOfWords > ClosureRequest::maxNumberOfCaptures)
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

    build().statistics().count(StatisticID::ClosuresInFrames);
    CellPointer cell = emitter<CellAllocationEmitter>().allocateInFrame(value, sizeOfClosureInFrame(SlimClosure::offsetOfCaptures(), count));
    Vector<LValue, 4> captureWords = captureWordsBySlot(value, request);
    size_t numberOfWordsStored = 0;
    if (generator().takesOpenForm(value, StateOfKnowledge::SlimInFrame)) {
        CodeGenerator::InitializationScope scope(generator(), CodeGenerator::FillsCellOfHeap::No);
        emitter<CellAllocationEmitter>().storeHeader(cell, bitOr(loadImmutable64(AOT_INSTANCE_OFFSET(slimClosureHeaders) + request.kind.slimClosureKind() * sizeof(uint64_t)), integer(stateOfFrameCellInHeader)));
        storeField(cell, m_heaps.AOTSlimClosure_word, closureWordOf(request));
        storeThatClosureInFrameHasNoCopyInHeap(m_out, m_heaps, cell, SlimClosure::offsetOfCaptures(), count);
    } else {
        numberOfWordsStored = std::min<size_t>(count, ClosureRequest::maxSlimClosureCapturesInRegisters);
        Vector<LValue, 6> operands;
        operands.append(m_out.wideNumber(closureNumberMentionedBy(request)));
        operands.append(cell.value());
        operands.append(captureWords.span().first(numberOfWordsStored));
        cell = CellPointer(emitter<RuntimeCallEmitter>().callStubWithoutSite(newSlimClosureInFrame(request.kind.slimClosureKind(), count), operands.span()).first);
    }
    {
        CodeGenerator::InitializationScope scope(generator(), CodeGenerator::FillsCellOfHeap::No);
        storeClosureCaptureWords(m_out, cell, captureWords.span(), m_heaps.AOTSlimClosure_captures, numberOfWordsStored);
    }
    return cell;
}

CellPointer ConstantEmitter::newClosureByStub(const ValueContext& value, const NewClosureRequest& request)
{
    m_out.noteForm(Form::Closed, StateOfKnowledge::FullOnHeap);
    unsigned executable = static_cast<unsigned>(FunctionTables::executableKind(request.kind));
    return callClosureStub(value, request, ClosureRequest::encode(0, executable, 0));
}

CellPointer ConstantEmitter::callClosureStub(const ValueContext& value, const NewClosureRequest& request, uint64_t requestWord)
{
    RELEASE_ASSERT(!ClosureRequest::closureNumberOf(requestWord) && !(requestWord >> ClosureRequest::allocationShift));
    switch (request.storage) {
    case AllocationStorage::Heap:
        break;
    case AllocationStorage::Frame:
        return failWithoutCell(value, InternalErrorKind::OpcodeWithoutCode);
    }

    unsigned count = request.numberOfWords;
    bool isSlim = ClosureRequest::isSlim(requestWord);
    uint32_t mention = closureNumberMentionedBy(request, isSlim ? 0 : static_cast<int32_t>(requestWord));
    LValue word = isSlim ? m_out.wideNumber(mention) : bitOr(zeroExtended(numberOf(mention)), Integer(m_out.shl(zeroExtended(numberOf(request.allocation.mention)).value(), ClosureRequest::allocationShift))).value();

    Vector<LValue, 4> captureWords = captureWordsBySlot(value, request);
    auto callOfStub = [&] {
        size_t numberInRegisters = std::min<size_t>(count, ClosureRequest::maxSlimClosureCapturesInRegisters);
        Vector<LValue, 6> operands;
        operands.append(word);
        operands.append(captureWords.span().first(numberInRegisters));
        CellPointer cell(callStub(value, isSlim ? AOT::newSlimClosure(request.kind.slimClosureKind(), count) : newClosure(count), operands.span()).first);
        if (numberInRegisters == count)
            return cell;

        {
            CodeGenerator::InitializationScope scope(generator());
            storeClosureCaptureWords(m_out, cell, captureWords.span(), m_heaps.AOTSlimClosure_captures, numberInRegisters);
        }
        emitter<BarrierEmitter>().fenceAfterInitialization(value);
        return cell;
    };
    if (!isSlim || !emitter<CellAllocationEmitter>().allocatesInline(value))
        return callOfStub();

    CellAllocationEmitter& cellAllocation = emitter<CellAllocationEmitter>();
    build().statistics().count(StatisticID::CellsAllocatedInline);
    const unsigned allocatorIndex = (count + 1) / 2;
    const ptrdiff_t offsetOfAllocator = AOT_INSTANCE_OFFSET(closures.slimClosureAllocators) + static_cast<ptrdiff_t>(allocatorIndex * sizeof(void*));
    LBasicBlock slowPath = newBlock();
    Join<CellPointer> join(m_out);
    Integer closureWord = closureWordOf(request);
    Integer header = loadImmutable64(AOT_INSTANCE_OFFSET(slimClosureHeaders) + request.kind.slimClosureKind() * sizeof(uint64_t));
    Pointer allocator(m_out.loadPtr(instanceField(m_heaps.entire(AbstractHeap::AllocatorState), offsetOfAllocator)));
    CellPointer cell = cellAllocation.allocateHeapCell(allocator, MarkedSpace::sizeStep * (allocatorIndex + 1), slowPath, false);
    {
        CodeGenerator::InitializationScope scope(generator());
        cellAllocation.storeHeader(cell, header);
        storeField(cell, m_heaps.AOTSlimClosure_word, closureWord);
        storeClosureCaptureWords(m_out, cell, captureWords.span(), m_heaps.AOTSlimClosure_captures);
    }
    emitter<BarrierEmitter>().fenceAfterInitialization(value);
    join.add(cell);
    m_out.appendTo(slowPath);
    join.add(callOfStub());
    return join.finish();
}

CellPointer ConstantEmitter::newFullClosure(const ValueContext& value, const NewClosureRequest& request)
{
    m_out.noteForm(Form::Open, request.storage == AllocationStorage::Frame ? StateOfKnowledge::FullInFrame : StateOfKnowledge::FullWithOwnStructure);
    CellAllocationEmitter& cellAllocation = emitter<CellAllocationEmitter>();
    unsigned structure = request.kind.closureStructureOfKind();
    unsigned executable = static_cast<unsigned>(FunctionTables::executableKind(request.kind));
    Vector<LValue, 4> captureWords = captureWordsBySlot(value, request);
    bool isOnHeap = false;
    CellPointer cell;
    switch (request.storage) {
    case AllocationStorage::Heap:
        cell = cellAllocation.allocate(value, request.allocation);
        isOnHeap = true;
        break;
    case AllocationStorage::Frame:
        cell = cellAllocation.allocateInFrame(value, sizeOfClosureInFrame(Closure::offsetOfCaptures(), request.numberOfWords));
        build().statistics().count(StatisticID::ClosuresInFrames);
        break;
    }

    {
        CodeGenerator::InitializationScope scope(generator(), CodeGenerator::FillsCellOfHeap(isOnHeap));
        if (!isOnHeap || structure) {
            Integer header = loadImmutable64(AOT_INSTANCE_OFFSET(closures.closureHeaders) + structure * sizeof(uint64_t));
            cellAllocation.storeHeader(cell, isOnHeap ? header : bitOr(header, integer(stateOfFrameCellInHeader)));
        }
        if (!isOnHeap)
            storeThatClosureInFrameHasNoCopyInHeap(m_out, m_heaps, cell, Closure::offsetOfCaptures(), request.numberOfWords);
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

Operand ConstantEmitter::captureOf(const NewClosureRequest& request, unsigned index) const
{
    RepresentationContext context { m_program, m_plan, m_generator.inputs().types, m_unit };
    return m_generator.getIn(request.captures[index], representationOfCapture(context, request.function, index));
}

bool ConstantEmitter::mayBeClosureOfFrame(ValueID captured)
{
    if (m_code.isConstant(captured))
        return false;
    if (!m_mayCaptureClosuresOfFrames)
        m_mayCaptureClosuresOfFrames = mayCaptureClosuresOfFrames(generator().facts(), m_code);
    if (!*m_mayCaptureClosuresOfFrames)
        return false;
    TypeID type = typeIDOf(captured);
    return !type || (!holdsOnlyPrimitives(type) && types().overlap(type, TypeTable::function));
}

Operand ConstantEmitter::broughtToHeap(const ValueContext& value, ValueID captured, Operand capture)
{
    auto cellBroughtToHeap = [&](CellPointer cell) {
        Word32 state(m_out.load8ZeroExt32(m_out.address(m_heaps.collectorState, cell.value(), JSCell::cellStateOffset())));
        return either<CellPointer>(notEqual(state, word32(frameCellState)), [&] {
            return cell;
        }, [&] {
            return callOperation<CellPointer>(value, RuntimeOperation::MoveClosureToHeap, { cell }, { .isOnRarePath = true });
        });
    };
    bool mayBeAllZeros = !typeIDOf(captured) || (typeOf(captured).primitives & (Primitive::Absent | Primitive::Unspecified));
    if (capture.is<CellPointer>()) {
        CellPointer cell = capture.as<CellPointer>();
        if (!mayBeAllZeros)
            return cellBroughtToHeap(cell);
        return selectByBranch<CellPointer>(isNotEmpty(boxCell(cell)), [&] {
            return cellBroughtToHeap(cell);
        }, [&] {
            return cell;
        });
    }

    Boxed boxed = capture.as<Boxed>();
    Condition holdsCell = isCell(boxed);
    if (mayBeAllZeros)
        holdsCell = logicalAnd(holdsCell, isNotEmpty(boxed));
    return selectByBranch<Boxed>(holdsCell, [&] {
        return boxCell(cellBroughtToHeap(unboxCell(boxed)));
    }, [&] {
        return boxed;
    });
}

LValue ConstantEmitter::wordKeptOnHeap(const ValueContext& value, ValueID kept)
{
    Boxed word = emitter<ConversionEmitter>().box(value, get(kept), typeOf(kept));
    return mayBeClosureOfFrame(kept) ? broughtToHeap(value, kept, word).value : word.value();
}

Vector<LValue, 4> ConstantEmitter::captureWordsBySlot(const ValueContext& value, const NewClosureRequest& request)
{
    unsigned count = request.captures.size();
    Vector<LValue, 4> words(FillWith { }, request.numberOfWords, nullptr);
    for (unsigned i = 0; i < count; ++i) {
        if (!closureHoldsWordForListedCapture(m_program, conventions(), request.function, i))
            continue;
        Operand capture = captureOf(request, i);
        if (request.storage == AllocationStorage::Heap && mayBeClosureOfFrame(request.captures[i]))
            capture = broughtToHeap(value, request.captures[i], capture);
        LType type = machineTypeOf(capture.representation);
        LValue word = type == Machine::Int32 ? m_out.zeroExtPtr(capture.value) : type == Machine::Double ? m_out.bitCast(capture.value, Machine::Int64) : capture.value;
        words[closureCaptureSlotOf(m_program, conventions(), request.function, i)] = word;
    }
    return words;
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
